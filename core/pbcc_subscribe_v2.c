/* -*- c-file-style:"stroustrup"; indent-tabs-mode: nil -*- */
#include "pubnub_internal.h"

#include "pubnub_version.h"
#include "pubnub_assert.h"
#include "pubnub_json_parse.h"
#include "pubnub_log.h"
#include "pubnub_url_encode.h"
#include "lib/pb_strnlen_s.h"
#include "pbcc_subscribe_v2.h"


#include <stdio.h>
#include <stdlib.h>


/** It has to contain the `t` field, with another `t` for the
    timetoken (as string) and `tr` for the region (integer) and the
    `m` field for the message(s) array.
*/
#define MIN_SUBSCRIBE_V2_RESPONSE_LENGTH 40

enum pubnub_res pbcc_subscribe_v2_prep(struct pbcc_context* p,
                                       char const*          channel,
                                       char const*          channel_group,
                                       unsigned*            heartbeat,
                                       char const*          filter_expr)
{
    char        region_str[20];
    char const* tr;

    if (NULL == channel) {
        if (NULL == channel_group) {
            return PNR_INVALID_CHANNEL;
        }
        channel = ",";
    }
    if (p->msg_ofs < p->msg_end) {
        return PNR_RX_BUFF_NOT_EMPTY;
    }

    if ('\0' == p->timetoken[0]) {
        p->timetoken[0] = '0';
        p->timetoken[1] = '\0';
        tr              = NULL;
    }
    else {
        snprintf(region_str, sizeof region_str, "%d", p->region);
        tr = region_str;
    }
    p->http_content_len = 0;
    p->msg_ofs = p->msg_end = 0;

    p->http_buf_len = snprintf(
        p->http_buf, sizeof p->http_buf, "/v2/subscribe/%s/", p->subscribe_key);
    APPEND_URL_ENCODED_M(p, channel);
    p->http_buf_len += snprintf(p->http_buf + p->http_buf_len,
                                sizeof p->http_buf - p->http_buf_len,
                                "/0?tt=%s&pnsdk=%s",
                                p->timetoken,
                                pubnub_uname());
    APPEND_URL_PARAM_M(p, "tr", tr, '&');
    APPEND_URL_PARAM_M(p, "channel-group", channel_group, '&');
    APPEND_URL_PARAM_M(p, "uuid", p->uuid, '&');
    APPEND_URL_PARAM_M(p, "auth", p->auth, '&');
    APPEND_URL_PARAM_ENCODED_M(p, "filter-expr", filter_expr, '&');
    APPEND_URL_OPT_PARAM_UNSIGNED_M(p, "heartbeat", heartbeat, '&');

    return PNR_STARTED;
}


enum pubnub_res pbcc_parse_subscribe_v2_response(struct pbcc_context* p)
{
    enum pbjson_object_name_parse_result jpresult;
    struct pbjson_elem                   el;
    struct pbjson_elem                   found;
    char*                                reply = p->http_reply;

    if (p->http_buf_len < MIN_SUBSCRIBE_V2_RESPONSE_LENGTH) {
        return PNR_FORMAT_ERROR;
    }
    if ((reply[0] != '{') || (reply[p->http_buf_len - 1] != '}')) {
        return PNR_FORMAT_ERROR;
    }

    el.start = p->http_reply;
    el.end   = p->http_reply + p->http_buf_len;
    jpresult = pbjson_get_object_value(&el, "t", &found);
    if (jonmpOK == jpresult) {
        struct pbjson_elem titel;
        if (jonmpOK == pbjson_get_object_value(&found, "t", &titel)) {
            size_t len = titel.end - titel.start - 2;
            if ((*titel.start != '"') || (titel.end[-1] != '"')) {
                PUBNUB_LOG_ERROR("Time token in response is not a string\n");
                return PNR_FORMAT_ERROR;
            }
            if (len >= sizeof p->timetoken) {
                PUBNUB_LOG_ERROR(
                    "Time token in response, length %lu, longer than max %lu\n",
                    (unsigned long)len,
                    (unsigned long)(sizeof p->timetoken - 1));
                return PNR_FORMAT_ERROR;
            }

            memcpy(p->timetoken, titel.start + 1, len);
            p->timetoken[len] = '\0';
        }
        else {
            PUBNUB_LOG_ERROR(
                "No timetoken value in subscribe V2 response found\n");
            return PNR_FORMAT_ERROR;
        }
        if (jonmpOK == pbjson_get_object_value(&found, "r", &titel)) {
            p->region = strtol(titel.start, NULL, 0);
        }
        else {
            PUBNUB_LOG_ERROR(
                "No region value in subscribe V2 response found\n");
            return PNR_FORMAT_ERROR;
        }
    }
    else {
        PUBNUB_LOG_ERROR(
            "No timetoken in subscribe V2 response found, error=%d\n", jpresult);
        return PNR_FORMAT_ERROR;
    }

    p->chan_ofs = p->chan_end = 0;

    /* We should optimize this to not look from the start again.
     */
    jpresult = pbjson_get_object_value(&el, "m", &found);
    if (jonmpOK == jpresult) {
        p->msg_ofs = (unsigned)(found.start - reply + 1);
        p->msg_end = (unsigned)(found.end - reply - 1);
    }
    else {
        PUBNUB_LOG_ERROR(
            "No message array subscribe V2 response found, error=%d\n", jpresult);
        return PNR_FORMAT_ERROR;
    }

    return PNR_OK;
}


struct pubnub_v2_message pbcc_get_msg_v2(struct pbcc_context* p)
{
    enum pbjson_object_name_parse_result jpresult;
    struct pbjson_elem                   el;
    struct pbjson_elem                   found;
    struct pubnub_v2_message             rslt;
    char const*                          start;
    char const*                          end;
    char const*                          seeker;

    memset(&rslt, 0, sizeof rslt);

    if (p->msg_ofs >= p->msg_end) {
        return rslt;
    }
    start = p->http_reply + p->msg_ofs;
    if (*start != '{') {
        PUBNUB_LOG_ERROR(
            "Message subscribe V2 response is not a JSON object\n");
        return rslt;
    }
    end    = p->http_reply + p->msg_end;
    seeker = pbjson_find_end_complex(start, end);
    if (seeker == end) {
        PUBNUB_LOG_ERROR(
            "Message subscribe V2 response has no end of JSON object\n");
        return rslt;
    }

    p->msg_ofs = (unsigned)(seeker - p->http_reply + 2);
    el.start   = start;
    el.end     = seeker;

    /* @todo We should iterate over elements of JSON message object,
       instead of looking from the start, again and again.
    */

    jpresult = pbjson_get_object_value(&el, "d", &found);
    if (jonmpOK == jpresult) {
        rslt.payload.ptr  = (char*)found.start;
        rslt.payload.size = found.end - found.start;
    }
    else {
        PUBNUB_LOG_ERROR("pbcc=%p: No message payload in subscribe V2 response "
                         "found, error=%d\n",
                         p,
                         jpresult);
        return rslt;
    }

    jpresult = pbjson_get_object_value(&el, "c", &found);
    if (jonmpOK == jpresult) {
        rslt.channel.ptr  = (char*)found.start + 1;
        rslt.channel.size = found.end - found.start - 2;
    }
    else {
        PUBNUB_LOG_ERROR("pbcc=%p: No message channel in subscribe V2 response "
                         "found, error=%d\n",
                         p,
                         jpresult);
        return rslt;
    }

    jpresult = pbjson_get_object_value(&el, "e", &found);
    if (jonmpOK == jpresult) {
        if (pbjson_elem_equals_string(&found, "1")) {
            rslt.message_type = pbsbSignal;
        }
        else {
            rslt.message_type = pbsbPublished;
        }
    }
    else {
        rslt.message_type = pbsbPublished;
    }
    
    jpresult = pbjson_get_object_value(&el, "p", &found);
    if (jonmpOK == jpresult) {
        struct pbjson_elem titel;
        if (jonmpOK == pbjson_get_object_value(&found, "t", &titel)) {
            if ((*titel.start != '"') || (titel.end[-1] != '"')) {
                PUBNUB_LOG_ERROR("Time token in response is not a string\n");
                return rslt;
            }
            rslt.tt.ptr  = (char*)titel.start + 1;
            rslt.tt.size = titel.end - titel.start - 2;
        }
        else {
            PUBNUB_LOG_ERROR(
                "No timetoken value in subscribe V2 response found\n");
            return rslt;
        }
    }
    else {
        PUBNUB_LOG_ERROR("No message publish timetoken in subscribe V2 "
                         "response found, error=%d\n",
                         jpresult);
        return rslt;
    }

    if (jonmpOK == pbjson_get_object_value(&el, "b", &found)) {
        rslt.match_or_group.ptr  = (char*)found.start;
        rslt.match_or_group.size = found.end - found.start;
    }

    if (jonmpOK == pbjson_get_object_value(&el, "u", &found)) {
        rslt.metadata.ptr  = (char*)found.start;
        rslt.metadata.size = found.end - found.start;
    }

    return rslt;
}
