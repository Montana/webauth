/*
 * mod_webdc
 */ 

#include "mod_webkdc.h"

/* attr list macros to make code easier to read and audit 
 * we don't need to check error codes since we are using
 * WA_F_NONE, which doesn't allocate any memory.
 */

#define ADD_STR(name,value) \
       webauth_attr_list_add_str(alist, name, value, 0, WA_F_NONE)

#define ADD_PTR(name,value, len) \
       webauth_attr_list_add(alist, name, value, len, WA_F_NONE)

#define ADD_TIME(name,value) \
       webauth_attr_list_add_time(alist, name, value, WA_F_NONE)

#define SET_APP_STATE(state,len)     ADD_PTR(WA_TK_APP_STATE, state, len)
#define SET_COMMAND(cmd)             ADD_STR(WA_TK_COMMAND, cmd)
#define SET_CRED_DATA(data, len)     ADD_PTR(WA_TK_CRED_DATA, data, len)
#define SET_CRED_TYPE(type)          ADD_STR(WA_TK_CRED_TYPE, type)
#define SET_CREATION_TIME(time)      ADD_TIME(WA_TK_CREATION_TIME, time)
#define SET_ERROR_CODE(code)         ADD_STR(WA_TK_ERROR_CODE, code)
#define SET_ERROR_MESSAGE(msg)       ADD_STR(WA_TK_ERROR_MESSAGE, msg)
#define SET_EXPIRATION_TIME(time)    ADD_TIME(WA_TK_EXPIRATION_TIME, time)
#define SET_INACTIVITY_TIMEOUT(to)   ADD_STR(WA_TK_INACTIVITY_TIMEOUT, to)
#define SET_SESSION_KEY(key,len)     ADD_PTR(WA_TK_SESSION_KEY, key, len)
#define SET_LASTUSED_TIME(time)      ADD_TIME(WA_TK_LASTUSED_TIME, time)
#define SET_PROXY_TYPE(type)         ADD_STR(WA_TK_PROXY_TYPE, type)
#define SET_PROXY_DATA(data,len)     ADD_PTR(WA_TK_PROXY_DATA, data, len)
#define SET_PROXY_SUBJECT(sub)       ADD_STR(WA_TK_PROXY_SUBJECT, sub)
#define SET_REQUEST_OPTIONS(ro)      ADD_STR(WA_TK_REQUEST_OPTIONS, ro)
#define SET_REQUESTED_TOKEN_TYPE(t)  ADD_STR(WA_TK_REQUESTED_TOKEN_TYPE, t)
#define SET_RETURN_URL(url)          ADD_STR(WA_TK_RETURN_URL, url)
#define SET_SUBJECT(s)               ADD_STR(WA_TK_SUBJECT, s)
#define SET_SUBJECT_AUTH(sa)         ADD_STR(WA_TK_SUBJECT_AUTH, sa)
#define SET_SUBJECT_AUTH_DATA(d,l)   ADD_PTR(WA_TK_SUBJECT_AUTH_DATA, d, l)
#define SET_TOKEN_TYPE(type)         ADD_STR(WA_TK_TOKEN_TYPE, type)
#define SET_WEBKDC_TOKEN(d,l)        ADD_PTR(WA_TK_WEBKDC_TOKEN, d, l)


/*
 * generate <errorResponse> message from error stored in rc
 */
static int
generate_errorResponse(MWK_REQ_CTXT *rc)
{
    char ec_buff[32];

    if (rc->error_code==0) {
        rc->error_code = WA_PEC_SERVER_FAILURE;
    }

    sprintf(ec_buff,"%d", rc->error_code);

    if (rc->error_message == NULL) {
        rc->error_message ="<this shouldn't be happening!>";
    }

    ap_rvputs(rc->r, 
              "<errorResponse><errorCode>",
              ec_buff,
              "</errorCode><errorMessage>",
              apr_xml_quote_string(rc->r->pool, rc->error_message, 0),
              "</errorMessage></errorResponse>",
              NULL);
    ap_rflush(rc->r);

    if (rc->need_to_log) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webkdc: %s: %s (%d)", rc->mwk_func, 
                     rc->error_message, rc->error_code);
    }
    return OK;
}


/*
 * sets error info in "rc" which will bubble up to original caller.
 * also returns MWK_ERROR to allow it to be used as return value
 */
static enum mwk_status
set_errorResponse(MWK_REQ_CTXT *rc, int ec, const char *message,
                  const char*mwk_func, int log)
{
    rc->error_code = ec;
    rc->error_message = message;
    rc->mwk_func = mwk_func;
    rc->need_to_log = log;
    return MWK_ERROR;
}

/* 
 * should only be called (and result used) while you have
 * the MWK_MUTEX_KEYRING mutex.
 */

static WEBAUTH_KEYRING *
get_keyring(MWK_REQ_CTXT *rc) {
    int status;
    static WEBAUTH_KEYRING *ring = NULL;

    if (ring != NULL) {
        return ring;
    }

    /* attempt to open up keyring */
    status = webauth_keyring_read_file(rc->sconf->keyring_path, &ring);
    if (status != WA_ERR_NONE) {
        mwk_log_webauth_error(rc->r, status, NULL,
                              "get_keyring", "webauth_keyring_read_file");
    } else {
        /* FIXME: should probably make sure we have at least one
           valid (not expired/postdated) key in the ring */
    }
    return ring;
}

/*
 * returns new attr list, or NULL if there was an error
 */

static WEBAUTH_ATTR_LIST *
new_attr_list(MWK_REQ_CTXT *rc, const char *mwk_func) 
{
    WEBAUTH_ATTR_LIST *alist = webauth_attr_list_new(32);
    if (alist == NULL) {
        set_errorResponse(rc, WA_PEC_SERVER_FAILURE, 
                          "no memory for attr list", mwk_func, 0);
    }
    return alist;
}

static enum mwk_status
make_token(MWK_REQ_CTXT *rc, WEBAUTH_ATTR_LIST *alist, time_t hint,
           char **out_token, int *out_len, 
           int base64_encode,
           const char *mwk_func)
{
    WEBAUTH_KEYRING *ring;
    char *buffer;
    int status, elen, olen;

    elen = webauth_token_encoded_length(alist);
    buffer = (char*)apr_palloc(rc->r->pool, elen);
    status = WA_ERR_NONE;

    mwk_lock_mutex(rc, MWK_MUTEX_KEYRING); /****** LOCKING! ************/

    ring = get_keyring(rc);
    if (ring != NULL) {
        status = webauth_token_create(alist, hint, buffer, &olen, elen, ring);
    }

    mwk_unlock_mutex(rc, MWK_MUTEX_KEYRING); /****** UNLOCKING! ************/

    if (ring == NULL) {
        return set_errorResponse(rc, WA_PEC_SERVER_FAILURE,
                                 "no keyring", mwk_func, 1);
    }

    if (status != WA_ERR_NONE) {
        mwk_log_webauth_error(rc->r, status, NULL, mwk_func,
                              "webauth_token_create");
        return set_errorResponse(rc, WA_PEC_SERVER_FAILURE,
                                 "token create failed", mwk_func, 0);
    }

    if (base64_encode) {
        *out_token = (char*) 
            apr_palloc(rc->r->pool, apr_base64_encode_len(olen));
        *out_len = apr_base64_encode(*out_token, buffer, olen);
    } else {
        *out_token = buffer;
        *out_len = olen;
    }
    return MWK_OK;
}


static enum mwk_status
make_token_with_key(MWK_REQ_CTXT *rc, 
                    WEBAUTH_KEY *key,
                    WEBAUTH_ATTR_LIST *alist, time_t hint,
                    char **out_token, int *out_len, 
                    int base64_encode,
                    const char *mwk_func)
{
    char *buffer;
    int status, elen, olen;

    elen = webauth_token_encoded_length(alist);
    buffer = (char*)apr_palloc(rc->r->pool, elen);

    status = webauth_token_create_with_key(alist, hint, buffer, 
                                           &olen, elen, key);

    if (status != WA_ERR_NONE) {
        mwk_log_webauth_error(rc->r, status, NULL, mwk_func,
                              "webauth_token_create_with_key");
        return set_errorResponse(rc, WA_PEC_SERVER_FAILURE,
                                 "token create failed", mwk_func, 0);
    }

    if (base64_encode) {
        *out_token = (char*) 
            apr_palloc(rc->r->pool, apr_base64_encode_len(olen));
        *out_len = apr_base64_encode(*out_token, buffer, olen);
    } else {
        *out_token = buffer;
        *out_len = olen;
    }
    return MWK_OK;
}


/*
 * log information about a bad element in XML and generate errorResponse
 */

static enum mwk_status
unknown_element(MWK_REQ_CTXT *rc, 
                const char *mwk_func, const char *parent, const char *u)
{
    char *msg = apr_psprintf(rc->r->pool, "unknown element in <%s>: <%s>",
                             parent, u);
    return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, msg, mwk_func, 1);
}


/*
 * concat all the text pieces together and return data, or
 * NULL if an error occured.
 */
char *
get_elem_text(MWK_REQ_CTXT *rc, apr_xml_elem *e, const char *mwk_func)
{
    MWK_STRING string;
    mwk_init_string(&string, rc->r->pool);

    if (e->first_cdata.first &&
        e->first_cdata.first->text) {
        apr_text *t;
         for (t = e->first_cdata.first; t != NULL; t = t->next) {
            mwk_append_string(&string, t->text, 0);
        }
    }

    if (!string.data || string.data[0] == '\0') {
        char *msg = apr_psprintf(rc->r->pool, "<%s> does not contain data",
                                 e->name);
        set_errorResponse(rc, WA_PEC_INVALID_REQUEST, msg, mwk_func, 1);
        return NULL;
    }
    return string.data;
}

/*
 * get an attr from an element. if required and not found, we
 * log an error and generate an errorResponse.
 */
static const char*
get_attr_value(MWK_REQ_CTXT *rc,apr_xml_elem *e, 
               const char *name, int required, const char *mwk_func)
{
    apr_xml_attr *a;

    for (a = e->attr; a != NULL; a = a->next) {
        if (strcmp(a->name, name) == 0) {
            return a->value;
        }
    }

    if (required) {
        char *msg = apr_psprintf(rc->r->pool, "can't find attr in <%s>: %s",
                                 e->name, name);
        set_errorResponse(rc, WA_PEC_INVALID_REQUEST, msg, mwk_func, 1);
    }
    return NULL;
}


/*
 * find an element in the specified element. if required and not found, we
 * log an error and generate an errorResponse.
 */
apr_xml_elem *
get_element(MWK_REQ_CTXT *rc,apr_xml_elem *e, 
            const char *name, int required, const char *mwk_func)
{
    apr_xml_elem *child;

    for (child = e->first_child; child != NULL; child = child->next) {
        if (strcmp(child->name, name) == 0) {
            return child;
        }
    }

    if (required) {
        char *msg = apr_psprintf(rc->r->pool, "can't find element in <%s>: %s",
                                 e->name, name);
        set_errorResponse(rc, WA_PEC_INVALID_REQUEST, msg, mwk_func, 1);
    }
    return NULL;
}

/*
 * search through subject credentials for a proxy-token of the requested
 * type.
 */
static MWK_PROXY_TOKEN *
find_proxy_token(MWK_REQ_CTXT *rc,
                 MWK_SUBJECT_CREDENTIAL *sub_cred, 
                 const char *type,
                 const char *mwk_func) 
{
    int i;
    char *msg;
    if (strcmp(sub_cred->type, "proxy") == 0) {
        for (i=0; i < sub_cred->u.proxy.num_proxy_tokens; i++) {
            if (strcmp(sub_cred->u.proxy.pt[i].proxy_type, type) == 0) {
                return  &sub_cred->u.proxy.pt[i];
            }
        }
    }
    msg = apr_psprintf(rc->r->pool, "need a proxy-token of type: %s", type);
    set_errorResponse(rc, WA_PEC_PROXY_TOKEN_REQUIRED, msg, mwk_func, 1);
    return NULL;
}

/*
 * parse a <serviceToken>, which should be base64-encoded.
 * logs all errors and generates errorResponse if need be.
 */
static enum mwk_status
parse_service_token(MWK_REQ_CTXT *rc, char *token, MWK_SERVICE_TOKEN *st)
{
    WEBAUTH_ATTR_LIST *alist;
    WEBAUTH_KEYRING *ring;
    int blen, status, i;
    const char *tt;
    static const char *mwk_func = "parse_service_token";
    enum mwk_status ms;

    ms = MWK_ERROR;

    if (token == NULL) {
        return set_errorResponse(rc, WA_PEC_SERVICE_TOKEN_INVALID, 
                                 "service token is NULL", mwk_func, 1);
    }

    blen = apr_base64_decode(token, token);
    status = WA_ERR_NONE;

    /* parse the token, TTL is zero because service-tokens don't have ttl,
     * just expiration
     */

    mwk_lock_mutex(rc, MWK_MUTEX_KEYRING); /****** LOCKING! ************/

    ring = get_keyring(rc);
    if (ring != NULL) {
        status = webauth_token_parse(token, blen, 0, ring, &alist);
    }

    mwk_unlock_mutex(rc, MWK_MUTEX_KEYRING); /****** UNLOCKING! ************/

    if (ring == NULL) {
        return set_errorResponse(rc, WA_PEC_SERVER_FAILURE, "no keyring",
                                 mwk_func, 1);
    }

    if (status != WA_ERR_NONE) {
        mwk_log_webauth_error(rc->r, status, NULL, mwk_func,
                              "webauth_token_parse");
        if (status == WA_ERR_TOKEN_EXPIRED) {
            return set_errorResponse(rc, WA_PEC_SERVICE_TOKEN_EXPIRED,
                                   "service token was expired", mwk_func, 0);
        } else if (status == WA_ERR_BAD_HMAC) {
            return set_errorResponse(rc, WA_PEC_SERVICE_TOKEN_INVALID,
                                   "can't decrypt service token", mwk_func, 0);
        } else {
            return set_errorResponse(rc, WA_PEC_SERVICE_TOKEN_INVALID,
                                   "error parsing token", mwk_func, 0);
        }
    }

    /* make sure its a service-token */
    tt = mwk_get_str_attr(alist, WA_TK_TOKEN_TYPE, rc->r, mwk_func, NULL);
    if ((tt == NULL) || (strcmp(tt, WA_TT_WEBKDC_SERVICE) != 0)) {
        set_errorResponse(rc, WA_PEC_SERVICE_TOKEN_INVALID, 
                               "not a service token", mwk_func, 1);
        goto cleanup;
    }

    /* pull out session key */
    status = webauth_attr_list_find(alist, WA_TK_SESSION_KEY, &i);
    if (i == -1) {
        set_errorResponse(rc, WA_PEC_SERVICE_TOKEN_INVALID, 
                               "missing session key", mwk_func, 1);
        goto cleanup;
    }
    st->key.length = alist->attrs[i].length;
    st->key.data = apr_palloc(rc->r->pool, st->key.length);
    memcpy(st->key.data, alist->attrs[i].value, st->key.length);
    st->key.type = WA_AES_KEY; /* HARCODED */

    /* pull out subject */
    status = webauth_attr_list_find(alist, WA_TK_SUBJECT, &i);
    if (i == -1) {
        set_errorResponse(rc, WA_PEC_SERVICE_TOKEN_INVALID, 
                               "missing subject", mwk_func, 1);
        goto cleanup;
    }
    st->subject = apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);
    ms = MWK_OK;

 cleanup:
    webauth_attr_list_free(alist);
    return ms;
}

/*
 * parse a proxy-token, which should be base64-encoded.
 * logs all errors and generates errorResponse if need be.
 */
static enum mwk_status
parse_webkdc_proxy_token(MWK_REQ_CTXT *rc, char *token, MWK_PROXY_TOKEN *pt)
{
    WEBAUTH_ATTR_LIST *alist;
    WEBAUTH_KEYRING *ring;
    int blen, status, i;
    enum mwk_status ms;
    const char *tt;
    static const char *mwk_func = "parse_webkdc_proxy_token";

    if (token == NULL) {
        return set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID,
                                 "proxy token is NULL", mwk_func, 1);
    }

    blen = apr_base64_decode(token, token);
    status = WA_ERR_NONE;
    ms = MWK_ERROR;

    /* parse the token, TTL is zero because proxy-tokens don't have ttl,
     * just expiration
     */

    mwk_lock_mutex(rc, MWK_MUTEX_KEYRING); /****** LOCKING! ************/

    ring = get_keyring(rc);
    if (ring != NULL) {
        status = webauth_token_parse(token, blen, 0, ring, &alist);
    }

    mwk_unlock_mutex(rc, MWK_MUTEX_KEYRING); /****** UNLOCKING! ************/

    if (ring == NULL) {
        return set_errorResponse(rc, WA_PEC_SERVER_FAILURE, "no keyring",
                                 mwk_func, 1);
    }

    if (status != WA_ERR_NONE) {
        mwk_log_webauth_error(rc->r, status, NULL, mwk_func,
                              "webauth_token_parse");
        if (status == WA_ERR_TOKEN_EXPIRED) {
            set_errorResponse(rc, WA_PEC_PROXY_TOKEN_EXPIRED,
                              "proxy token was expired", mwk_func, 0);
        } else if (status == WA_ERR_BAD_HMAC) {
            set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID,
                              "can't decrypt proxy token", mwk_func, 0);
        } else {
            set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID,
                              "error parsing token", mwk_func, 0);
        }
        return MWK_ERROR;
    }

    /* make sure its a proxy-token */
    tt = mwk_get_str_attr(alist, WA_TK_TOKEN_TYPE, rc->r, mwk_func, NULL);
    if ((tt == NULL) || (strcmp(tt, WA_TT_WEBKDC_PROXY) != 0)) {
        set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID, 
                          "not a webkdc-proxy token", mwk_func, 1);
        goto cleanup;
    }

    /* pull out proxy-data key */
    status = webauth_attr_list_find(alist, WA_TK_PROXY_DATA, &i);
    if (i == -1) {
        set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID, 
                          "missing proxy data", mwk_func, 1);
        goto cleanup;
    }
    pt->proxy_data_len = alist->attrs[i].length;
    pt->proxy_data = apr_palloc(rc->r->pool, pt->proxy_data_len);
    memcpy(pt->proxy_data, alist->attrs[i].value, pt->proxy_data_len);

    /* pull out subject */
    status = webauth_attr_list_find(alist, WA_TK_SUBJECT, &i);
    if (i == -1) {
        set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID, 
                          "missing subject", mwk_func, 1);
        goto cleanup;
    }
    pt->subject = apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);

    /* pull out proxy type */
    status = webauth_attr_list_find(alist, WA_TK_PROXY_TYPE, &i);
    if (i == -1) {
        set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID, 
                          "missing proxy type", mwk_func, 1);
        goto cleanup;
    }
    pt->proxy_type = apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);

    /* pull out proxy subject */
    status = webauth_attr_list_find(alist, WA_TK_PROXY_SUBJECT, &i);
    if (i == -1) {
        set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID, 
                          "missing proxy subject type", mwk_func, 1);
        goto cleanup;
    }
    pt->proxy_subject = apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);

    /* pull out expiration */
    status = webauth_attr_list_get_time(alist, WA_TK_EXPIRATION_TIME,
                                        &pt->expiration, WA_F_NONE);
    if (status != WA_ERR_NONE) {
        set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID, 
                          "missing expiration", mwk_func, 1);
        goto cleanup;
    }

    ms = MWK_OK;

 cleanup:
    webauth_attr_list_free(alist);
    return ms;
}


/*
 * parse a login-token, which should be base64-encoded.
 * return 1 on success, 0 on error.
 * logs all errors and generates errorResponse if need be.
 */
static enum mwk_status
parse_login_token(MWK_REQ_CTXT *rc, char *token,
                     MWK_LOGIN_TOKEN *lt)
{
    WEBAUTH_ATTR_LIST *alist;
    WEBAUTH_KEYRING *ring;
    int blen, status, i;
    enum mwk_status ms;
    const char *tt;
    static const char *mwk_func = "parse_login_token";

    if (token == NULL) {
        return set_errorResponse(rc, WA_PEC_LOGIN_TOKEN_INVALID,
                                 "login token is NULL", mwk_func, 1);
    }
    
    blen = apr_base64_decode(token, token);
    status = WA_ERR_NONE;
    ms = MWK_ERROR;

    /* parse the token, with a TTL */

    mwk_lock_mutex(rc, MWK_MUTEX_KEYRING); /****** LOCKING! ************/

    ring = get_keyring(rc);
    if (ring != NULL) {
        status = webauth_token_parse(token, blen,
                                     rc->sconf->token_max_ttl,
                                     ring, &alist);
    }

    mwk_unlock_mutex(rc, MWK_MUTEX_KEYRING); /****** UNLOCKING! ************/

    if (ring == NULL) {
        return set_errorResponse(rc, WA_PEC_SERVER_FAILURE, "no keyring",
                                 mwk_func, 1);
    }

    if (status != WA_ERR_NONE) {
        mwk_log_webauth_error(rc->r, status, NULL, mwk_func,
                              "webauth_token_parse");
        if (status == WA_ERR_TOKEN_STALE) {
            set_errorResponse(rc, WA_PEC_LOGIN_TOKEN_STALE,
                              "login token was stale", mwk_func, 0);
        } else if (status == WA_ERR_BAD_HMAC) {
            set_errorResponse(rc, WA_PEC_LOGIN_TOKEN_INVALID,
                              "can't decrypt login token", mwk_func, 0);
        } else {
            set_errorResponse(rc, WA_PEC_LOGIN_TOKEN_INVALID,
                              "error parsing token", mwk_func, 0);
        }
        return MWK_ERROR;
    }

    /* make sure its a login-token */
    tt = mwk_get_str_attr(alist, WA_TK_TOKEN_TYPE, rc->r, mwk_func, NULL);
    if ((tt == NULL) || (strcmp(tt, WA_TT_LOGIN) != 0)) {
        set_errorResponse(rc, WA_PEC_LOGIN_TOKEN_INVALID, 
                          "not a login token", mwk_func, 1);
        goto cleanup;
    }

    /* pull out username */
    status = webauth_attr_list_find(alist, WA_TK_USERNAME, &i);
    if (i == -1) {
        set_errorResponse(rc, WA_PEC_LOGIN_TOKEN_INVALID, 
                          "missing username", mwk_func, 1);
        goto cleanup;
    }
    lt->username = apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);

    /* pull out password */
    status = webauth_attr_list_find(alist, WA_TK_PASSWORD, &i);
    if (i == -1) {
        set_errorResponse(rc, WA_PEC_LOGIN_TOKEN_INVALID, 
                          "missing password", mwk_func, 1);
        goto cleanup;
    }
    lt->password = apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);
    ms = MWK_OK;

 cleanup:
    webauth_attr_list_free(alist);
    return ms;
}

/*
 * parse a <requestToken> from a POST, which should be base64-encoded.
 * return 1 on success, 0 on error.
 * logs all errors and generates errorResponse if need be.
 */
static enum mwk_status
parse_request_token(MWK_REQ_CTXT *rc, 
                    char *token,
                    MWK_SERVICE_TOKEN *st,
                    MWK_REQUEST_TOKEN *rt,
                    int cmd_only)
{
    WEBAUTH_ATTR_LIST *alist;
    int blen, status, i;
    enum mwk_status ms;
    const char *tt;
    static const char *mwk_func = "parse_xml_request_token";

    if (token == NULL) {
        return set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_INVALID,
                                 "request token is NULL", mwk_func, 1);
    }

    blen = apr_base64_decode(token, token);

    ms = MWK_ERROR;

    /* parse the token, use TTL  */
    status = webauth_token_parse_with_key(token, blen, 
                                          rc->sconf->token_max_ttl,
                                          &st->key, &alist);
    if (status != WA_ERR_NONE) {
        mwk_log_webauth_error(rc->r, status, NULL, "parse_xml_request_token", 
                              "webauth_token_parse");
        if (status == WA_ERR_TOKEN_STALE) {
            set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_STALE,
                              "request token was stale", mwk_func, 0);
        } else if (status == WA_ERR_BAD_HMAC) {
            set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_INVALID,
                              "can't decrypt request token", mwk_func, 0);
        } else {
            set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_INVALID,
                              "error parsing token", mwk_func, 0);
        }
        return MWK_ERROR;
    }

    /* make sure its a request-token */
    tt = mwk_get_str_attr(alist, WA_TK_TOKEN_TYPE, rc->r, mwk_func, NULL);
    if ((tt == NULL) || (strcmp(tt, WA_TT_REQUEST) != 0)) {
        return set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_INVALID, 
                                 "not a request token", mwk_func, 1);
    }

    if (cmd_only) {
        /* pull out command */
        status = webauth_attr_list_find(alist, WA_TK_COMMAND, &i);
        if (i == -1) {
            set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_INVALID, 
                              "missing command", mwk_func, 1);
            goto cleanup;
        }
        rt->cmd = apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);
        ms = MWK_OK;
        goto cleanup;
    }

    /* else expecting full request-token */

    /* pull out optional WA_TK_APP_STATE */
    status = webauth_attr_list_find(alist, WA_TK_APP_STATE, &i);
    if (i != -1) {
        rt->app_state_len = alist->attrs[i].length;
        rt->app_state = apr_palloc(rc->r->pool, rt->app_state_len);
        memcpy(rt->app_state, alist->attrs[i].value, rt->app_state_len);
    } else {
        rt->app_state_len = 0;
        rt->app_state = NULL;
    }

    /* pull out return-url */
    status = webauth_attr_list_find(alist, WA_TK_RETURN_URL, &i);
    if (i == -1) {
        set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_INVALID, 
                          "missing return url", mwk_func, 1);
        goto cleanup;
    }
    rt->return_url = apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);

    /* pull out request-options, they are optional */
    status = webauth_attr_list_find(alist, WA_TK_REQUEST_OPTIONS, &i);
    if (i == -1) {
        rt->request_options = "";
    } else {
        rt->request_options = 
            apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);
    }

    /* pull out requested-token-type */
    status = webauth_attr_list_find(alist, WA_TK_REQUESTED_TOKEN_TYPE, &i);
    if (i == -1) {
        set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_INVALID, 
                          "missing requested token type", mwk_func, 1);
        goto cleanup;
    }

    rt->requested_token_type = 
        apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);

    if (strcmp(rt->requested_token_type, "id") == 0) {
        /* pull out subject-auth-type */
        status = webauth_attr_list_find(alist, WA_TK_SUBJECT_AUTH, &i);
        if (i == -1) {
            set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_INVALID, 
                              "missing subject auth type", mwk_func, 1);
            goto cleanup;
        }

        rt->u.subject_auth_type = 
            apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);
    } else if (strcmp(rt->requested_token_type, "proxy") == 0) {
        /* pull out proxy-type */
        status = webauth_attr_list_find(alist, WA_TK_PROXY_TYPE, &i);
        if (i == -1) {
            set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_INVALID, 
                              "missing proxy type", mwk_func, 1);
            goto cleanup;
        }

        rt->u.proxy_type = 
            apr_pstrdup(rc->r->pool, (char*)alist->attrs[i].value);
    } else {
        char *msg = apr_psprintf(rc->r->pool, 
                                 "unknown requested-token-typee: %s",
                                 rt->requested_token_type);
        set_errorResponse(rc, WA_PEC_REQUEST_TOKEN_INVALID, msg, 
                          mwk_func, 1);
        goto cleanup;
    }

    ms = MWK_OK;

 cleanup:
    webauth_attr_list_free(alist);
    return ms;
}

/*
 */
static enum mwk_status
parse_requesterCredential(MWK_REQ_CTXT *rc, apr_xml_elem *e, 
                          MWK_REQUESTER_CREDENTIAL *req_cred,
                          int expecting_reqToken)
{
    int status;
    static const char*mwk_func = "parse_requesterCredential";
    const char *at = get_attr_value(rc, e, "type",  1, mwk_func);

    if (at == NULL) {
        return MWK_ERROR;
    }

    req_cred->type = apr_pstrdup(rc->r->pool, at);

    if (strcmp(at, "service") == 0) {
        const char *token = get_elem_text(rc, e, mwk_func);
        if (token == NULL) {
            return MWK_ERROR;
        }

        if (!parse_service_token(rc, (char*)token, &req_cred->u.st)) {
            return MWK_ERROR;
        }
        /* pull out subject from service token */
        req_cred->subject = req_cred->u.st.subject;
        return MWK_OK;
    } else if (strcmp(at, "krb5") == 0) {
        const char *req;
        int blen;
        char *bin_req, *client_principal;
        WEBAUTH_KRB5_CTXT *ctxt = mwk_get_webauth_krb5_ctxt(rc->r, mwk_func);
        /* mwk_get_webauth_krb5_ctxt already logged error */
        if (ctxt == NULL) {
            return set_errorResponse(rc, WA_PEC_SERVER_FAILURE, 
                                     "server failure", mwk_func, 0);
        }

        req = get_elem_text(rc, e, mwk_func);
        if (req == NULL) {
            return MWK_ERROR;
        }

        bin_req = (char*)apr_palloc(rc->r->pool, 
                                    apr_base64_decode_len(req));
        blen = apr_base64_decode(bin_req, req);

        status = webauth_krb5_rd_req(ctxt, bin_req, blen,
                                     rc->sconf->keytab_path,
                                     &client_principal, 0);

        if (status != WA_ERR_NONE) {
            char *msg = mwk_webauth_error_message(rc->r, status, ctxt,
                                                  "webauth_krb5_rd_req");
            set_errorResponse(rc, WA_PEC_REQUESTER_KRB5_CRED_INVALID, msg,
                              mwk_func, 1);
            webauth_krb5_free(ctxt);
            return MWK_ERROR;
        }
        webauth_krb5_free(ctxt);
        req_cred->subject = apr_pstrcat(rc->r->pool, "krb5:", client_principal,
                                        NULL);
        free(client_principal);
        return MWK_OK;
    } else {
        char *msg = apr_psprintf(rc->r->pool, 
                                 "unknown <requesterCredential> type: %s",
                                 at);
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 msg, mwk_func, 1);
    }
    return MWK_ERROR;
}


/*
 */
static enum mwk_status
parse_subjectCredential(MWK_REQ_CTXT *rc, apr_xml_elem *e, 
                        MWK_SUBJECT_CREDENTIAL *sub_cred)
{
    static const char*mwk_func = "parse_subjectCredential";

    const char *at = get_attr_value(rc, e, "type",  1, mwk_func);

    if (at == NULL) {
        return MWK_ERROR;
    }

    sub_cred->type = apr_pstrdup(rc->r->pool, at);

    if (strcmp(at, "proxy") == 0) {
        int n  = 0;
        apr_xml_elem *child;
        /* attempt to parse each proxy token */
        for (child = e->first_child; child; child = child->next) {
            if (strcmp(child->name, "proxyToken") == 0) {
                char *token = get_elem_text(rc, child, mwk_func);
                if (token == NULL)
                    return MWK_ERROR;
                if (!parse_webkdc_proxy_token(rc, token, 
                                              &sub_cred->u.proxy.pt[n]))
                    return MWK_ERROR;
                n++;
            } else {
                unknown_element(rc, mwk_func, e->name, child->name);
                return MWK_ERROR;
            }
        }
        sub_cred->u.proxy.num_proxy_tokens = n;
    } else if (strcmp(at, "login") == 0) {
        char *token;
        apr_xml_elem *login_token = get_element(rc, e,
                                                "loginToken", 1, mwk_func);
        if (login_token == NULL)
            return MWK_ERROR;

        token = get_elem_text(rc, login_token, mwk_func);
        if (token == NULL) {
            return MWK_ERROR;
        }

        if (!parse_login_token(rc, token, &sub_cred->u.lt)) {
            return MWK_ERROR;
        }
    } else {
        char *msg = apr_psprintf(rc->r->pool, 
                                 "unknown <subjectCredential> type: %s",
                                 at);
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, msg,
                                 mwk_func, 1);
    }
    return MWK_OK;
}

static enum mwk_status
create_service_token_from_req(MWK_REQ_CTXT *rc, 
                              MWK_REQUESTER_CREDENTIAL *req_cred,
                              MWK_RETURNED_TOKEN *rtoken)
{
    static const char *mwk_func="create_service_token_from_req";
    unsigned char session_key[WA_AES_128];
    int status, len;
    enum mwk_status ms;
    time_t creation, expiration;
    WEBAUTH_ATTR_LIST *alist;

    ms = MWK_ERROR;

    /* only create service tokens from krb5 creds */
    if (strcmp(req_cred->type, "krb5") != 0) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "can only create service-tokens with "
                                 "<requesterCredential> of type krb",
                                 mwk_func, 1);
    }

    /*FIXME: ACL CHECK: subject allowed to get a service token? */

    status = webauth_random_key(session_key, sizeof(session_key));

    if (status != WA_ERR_NONE) {
        mwk_log_webauth_error(rc->r, status, NULL, mwk_func,
                              "webauth_random_key");
        return set_errorResponse(rc, WA_PEC_SERVER_FAILURE, 
                                 "can't generate session key", mwk_func, 0);
    }

    time(&creation);
    expiration = creation + rc->sconf->service_token_lifetime;

    alist = new_attr_list(rc, mwk_func);
    if (alist == NULL) {
        return MWK_ERROR;
    }

    SET_TOKEN_TYPE(WA_TT_WEBKDC_SERVICE);
    SET_SESSION_KEY(session_key, sizeof(session_key));
    SET_SUBJECT(req_cred->subject);
    SET_CREATION_TIME(creation);
    SET_EXPIRATION_TIME(expiration);

    ms = make_token(rc, alist, creation,
                    (char**)&rtoken->token_data, &len, 1, mwk_func);

    webauth_attr_list_free(alist);

    if (!ms)
        return MWK_ERROR;

    rtoken->expires = apr_psprintf(rc->r->pool, "%d", (int)expiration);

    len = sizeof(session_key);
    rtoken->session_key = (char*) 
        apr_palloc(rc->r->pool, apr_base64_encode_len(len));
    apr_base64_encode((char*)rtoken->session_key, session_key, len);

    return MWK_OK;
}


/*
 * sad is allocated from request pool
 */
static enum mwk_status
get_krb5_sad(MWK_REQ_CTXT *rc, 
             MWK_REQUESTER_CREDENTIAL *req_cred,
             MWK_PROXY_TOKEN *sub_pt,
             unsigned char **sad,
             int *sad_len,
             const char *mwk_func)
{
    WEBAUTH_KRB5_CTXT *ctxt;
    int status;
    char *server_principal;
    unsigned char *temp_sad;
    enum mwk_status ms;

    ctxt = mwk_get_webauth_krb5_ctxt(rc->r, mwk_func);
    if (ctxt == NULL) {
        /* mwk_get_webauth_krb5_ctxt already logged error */
        return set_errorResponse(rc, WA_PEC_SERVER_FAILURE, 
                                      "server failure (webauth_krb5_new)", 
                                      mwk_func, 0);
    }

    status = webauth_krb5_init_via_tgt(ctxt, sub_pt->proxy_data, 
                                       sub_pt->proxy_data_len, NULL);

    if (status != WA_ERR_NONE) {
        char *msg = mwk_webauth_error_message(rc->r,
                                              status, ctxt,
                                              "webauth_krb5_export_ticket");
        webauth_krb5_free(ctxt);
        /* FIXME: probably need to examine errors a little more closely
         *        to determine if we should return a proxy-token error
         *        or a server-failure.
         */
        return set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID, msg, 
                                 mwk_func, 1);
    }

    server_principal = req_cred->u.st.subject;
    if (strncmp(server_principal, "krb5:", 5) == 0) {
        server_principal += 5;
    }

    status = webauth_krb5_mk_req(ctxt, server_principal, &temp_sad, sad_len);

    if (status != WA_ERR_NONE) {
        char *msg = mwk_webauth_error_message(rc->r, status, ctxt,
                                              "webauth_krb5_mk_req");
        /* FIXME: probably need to examine errors a little more closely
         *        to determine if we should return a proxy-token error
         *        or a server-failure.
         */
        set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID, msg, mwk_func, 1);
        ms = MWK_ERROR;
    } else {
        *sad = apr_palloc(rc->r->pool, *sad_len);
        memcpy(*sad,  temp_sad, *sad_len);
        free(temp_sad);
        ms = MWK_OK;
    }

    webauth_krb5_free(ctxt);
    return ms;
}


/*
 */
static enum mwk_status
create_error_token_from_req(MWK_REQ_CTXT *rc, 
                            int error_code,
                            const char *error_message,
                            MWK_REQUESTER_CREDENTIAL *req_cred,
                            MWK_RETURNED_TOKEN *rtoken)
{
    static const char *mwk_func="create_error_token_from_req";
    WEBAUTH_ATTR_LIST *alist;
    time_t creation;
    enum mwk_status ms;
    int tlen;

    alist = new_attr_list(rc, mwk_func);
    if (alist == NULL)
        return MWK_ERROR;

    time(&creation);

    SET_TOKEN_TYPE(WA_TT_ERROR);
    SET_CREATION_TIME(creation);
    SET_ERROR_CODE(apr_psprintf(rc->r->pool, "%d", error_code));
    SET_ERROR_MESSAGE(error_message);

    ms = make_token_with_key(rc, &req_cred->u.st.key,
                             alist, creation,
                             (char**)&rtoken->token_data, 
                             &tlen, 1, mwk_func);
    webauth_attr_list_free(alist);
    return ms;
}

/*
 */
static enum mwk_status
create_id_token_from_req(MWK_REQ_CTXT *rc, 
                         const char *auth_type,
                         MWK_REQUESTER_CREDENTIAL *req_cred,
                         MWK_SUBJECT_CREDENTIAL *sub_cred,
                         MWK_RETURNED_TOKEN *rtoken)
{
    static const char *mwk_func="create_id_token_from_req";
    int tlen, sad_len;
    enum mwk_status ms;
    time_t creation, expiration;
    WEBAUTH_ATTR_LIST *alist;
    MWK_PROXY_TOKEN *sub_pt;
    const char *subject;
    unsigned char *sad;

    ms = MWK_ERROR;

    /* make sure auth_type is not NULL */
    if (auth_type == NULL) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "auth type is NULL",
                                 mwk_func, 1);
    }
    
    /* only create id tokens from service creds */
    if (strcmp(req_cred->type, "service") != 0) {

        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "can only create id-tokens with "
                                 "<requesterCredential> of type service",
                                 mwk_func, 1);
    }

    /* make sure we have a subject cred with a type='proxy' */
    if (strcmp(sub_cred->type, "proxy") != 0) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "can only create id-tokens with "
                                 "<subjectCredential> of type proxy",
                                 mwk_func, 1);
    }

    /* FIXME: ACL CHECK: requester allowed to get an id token
     *        using subject cred?
     */

    sad = NULL;
    subject = NULL;

    if (strcmp(auth_type, "webkdc") == 0) {
        /* FIXME: are we going to have a webkc proxy type? */
        sub_pt = find_proxy_token(rc, sub_cred, "krb5", mwk_func);
        if (sub_pt == NULL)
            return MWK_ERROR;
        subject = sub_pt->subject;
    } else if (strcmp(auth_type, "krb5") == 0) {
        /* find a proxy-token of the right type */
        sub_pt = find_proxy_token(rc, sub_cred, "krb5", mwk_func);
        if (sub_pt == NULL)
            return MWK_ERROR;
        if (!get_krb5_sad(rc, req_cred, sub_pt, &sad, &sad_len, mwk_func)) {
            return MWK_ERROR;
        }

    } else {
        char *msg = apr_psprintf(rc->r->pool, "invalid authenticator type %s",
                                 auth_type);
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, msg, mwk_func, 1);
    }

    alist = new_attr_list(rc, mwk_func);
    if (alist == NULL)
        return MWK_ERROR;

    time(&creation);
    /* expiration comes from expiration of proxy-token */
    expiration = sub_pt->expiration;

    SET_TOKEN_TYPE(WA_TT_ID);
    SET_SUBJECT_AUTH(auth_type);
    if (subject != NULL) {
        SET_SUBJECT(subject);
    }
    if (sad != NULL) {
        SET_SUBJECT_AUTH_DATA(sad, sad_len);
    }
    SET_CREATION_TIME(creation);
    SET_EXPIRATION_TIME(expiration);

    ms = make_token_with_key(rc, &req_cred->u.st.key,
                             alist, creation,
                             (char**)&rtoken->token_data, 
                             &tlen, 1, mwk_func);
    webauth_attr_list_free(alist);

    return ms;
}

/*
 */
static enum mwk_status
create_proxy_token_from_req(MWK_REQ_CTXT *rc, 
                               const char *proxy_type,
                               MWK_REQUESTER_CREDENTIAL *req_cred,
                               MWK_SUBJECT_CREDENTIAL *sub_cred,
                               MWK_RETURNED_TOKEN *rtoken)
{
    static const char *mwk_func="create_proxy_token_from_req";
    int tlen, wkdc_len;
    enum mwk_status ms;
    time_t creation, expiration;
    WEBAUTH_ATTR_LIST *alist;
    MWK_PROXY_TOKEN *sub_pt;
    unsigned char *wkdc_token;

    ms = MWK_ERROR;

    /* make sure proxy_type is not NULL */
    if (proxy_type == NULL) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "proxy type is NULL",
                                 mwk_func, 1);
    }
    
    /* only create proxy tokens from service creds */
    if (strcmp(req_cred->type, "service") != 0) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "can only create proxy-tokens with "
                                 "<requesterCredential> of type service",
                                 mwk_func, 1);
    }

    /* make sure we have a subject cred with a type='proxy' */
    if (strcmp(sub_cred->type, "proxy") != 0) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "can only create proxy-tokens with "
                                 "<subjectCredential> of type proxy",
                                 mwk_func, 1);
    }

    /* FIXME: ACL CHECK: requester allowed to get a proxy token
     *        using subject cred?
     */


    /* make sure we are creating a proxy-tyoken that has
       the same type as the proxy-token we using to create it */
    sub_pt = find_proxy_token(rc, sub_cred, proxy_type, mwk_func);
    if (sub_pt == NULL) 
        return MWK_ERROR;

    /* create the webkdc-proxy-token first, using existing proxy-token */
    alist = new_attr_list(rc, mwk_func);
    if (alist == NULL)
        return MWK_ERROR;

    time(&creation);

    /* expiration comes from expiration of proxy-token */
    expiration =  sub_pt->expiration;

    /* make sure to use subject from service-token for new proxy-subject */
    SET_TOKEN_TYPE(WA_TT_WEBKDC_PROXY);
    SET_CREATION_TIME(creation);
    SET_EXPIRATION_TIME(expiration);
    SET_PROXY_TYPE(sub_pt->proxy_type);
    SET_PROXY_SUBJECT(req_cred->u.st.subject);
    SET_SUBJECT(sub_pt->subject);
    SET_PROXY_DATA(sub_pt->proxy_data, sub_pt->proxy_data_len);

    ms = make_token(rc, alist, creation,
                       (char**)&wkdc_token, &wkdc_len, 0, mwk_func);
    webauth_attr_list_free(alist);

    if (!ms)
        return MWK_ERROR;

    /* now create the proxy-token */
    alist = new_attr_list(rc, mwk_func);
    if (alist == NULL)
        return MWK_ERROR;

    SET_TOKEN_TYPE(WA_TT_PROXY);
    SET_PROXY_TYPE(sub_pt->proxy_type);
    SET_SUBJECT(sub_pt->subject);
    SET_WEBKDC_TOKEN(wkdc_token, wkdc_len);
    SET_CREATION_TIME(creation);
    SET_EXPIRATION_TIME(expiration);

    ms = make_token_with_key(rc, &req_cred->u.st.key,
                             alist, creation,
                             (char**)&rtoken->token_data, 
                             &tlen, 1, mwk_func);
    webauth_attr_list_free(alist);
    return ms;
}

/*
 */
static enum mwk_status
create_cred_token_from_req(MWK_REQ_CTXT *rc, 
                           apr_xml_elem *e,
                           MWK_REQUESTER_CREDENTIAL *req_cred,
                           MWK_SUBJECT_CREDENTIAL *sub_cred,
                           MWK_RETURNED_TOKEN *rtoken)
{
    static const char *mwk_func="create_cred_token_from_req";
    int tlen,status, ticket_len;
    time_t creation, expiration, ticket_expiration;
    WEBAUTH_ATTR_LIST *alist;
    apr_xml_elem *credential_type, *server_principal;
    const char *ct, *sp;
    WEBAUTH_KRB5_CTXT *ctxt;
    MWK_PROXY_TOKEN *sub_pt;
    unsigned char *ticket;
    enum mwk_status ms;

    /* only create cred tokens from service creds */
    if (strcmp(req_cred->type, "service") != 0 ) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "can only create cred-tokens with "
                                 "<requesterCredential> of type service",
                                 mwk_func, 1);
    }

    /* make sure we have a subject cred with a type='proxy' */
    if (strcmp(sub_cred->type, "proxy") != 0) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "can only create cred-tokens with "
                                 "<subjectCredential> of type proxy",
                                 mwk_func, 1);
    }

    /* FIXME: ACL CHECK: requester allowed to get a cred token
     *        using subject cred?
     */

    credential_type = get_element(rc, e, "credentialType", 1, mwk_func);

    if (credential_type == NULL)
        return MWK_ERROR;

    ct = get_elem_text(rc, credential_type, mwk_func);

    if (ct == NULL) 
        return MWK_ERROR;

    server_principal = get_element(rc, e, "serverPrincipal", 1, mwk_func);

    if (server_principal == NULL)
        return MWK_ERROR;

    sp = get_elem_text(rc, server_principal, mwk_func);

    if (sp == NULL) 
        return MWK_ERROR;

    /* make sure we are creating a cred-token that has
       the same type as the proxy-token we are using to create it */
    sub_pt = find_proxy_token(rc, sub_cred, ct, mwk_func);
    if (sub_pt == NULL)
        return MWK_ERROR;

    /* try to get the credentials  */
    ctxt = mwk_get_webauth_krb5_ctxt(rc->r, mwk_func);
    if (ctxt == NULL) {
        return set_errorResponse(rc, WA_PEC_SERVER_FAILURE, 
                                      "server failure", mwk_func, 0);
    }

    status = webauth_krb5_init_via_tgt(ctxt,
                                       sub_pt->proxy_data, 
                                       sub_pt->proxy_data_len,
                                       NULL);
    if (status != WA_ERR_NONE) {
        char *msg = mwk_webauth_error_message(rc->r,
                                              status, ctxt,
                                              "webauth_krb5_init_via_tgt");
        webauth_krb5_free(ctxt);
        /* FIXME: probably need to examine errors a little more closely
         *        to determine if we should return a proxy-token error
         *        or a server-failure.
         */
        return set_errorResponse(rc, WA_PEC_PROXY_TOKEN_INVALID, msg,
                                 mwk_func, 1);
    }

    /* now try and export a ticket */
    status = webauth_krb5_export_ticket(ctxt,
                                        (char*)sp,
                                        &ticket,
                                        &ticket_len,
                                        &ticket_expiration);

    if (status != WA_ERR_NONE) {
        char *msg = mwk_webauth_error_message(rc->r,
                                              status, ctxt,
                                              "webauth_krb5_export_ticket");
        webauth_krb5_free(ctxt);
        return set_errorResponse(rc, WA_PEC_GET_CRED_FAILURE, 
                                 msg, mwk_func, 1);
    }

    webauth_krb5_free(ctxt);

    /* now create the cred-token */
    alist = new_attr_list(rc, mwk_func);
    if (alist == NULL)
        return MWK_ERROR;

    time(&creation);

    /* expiration comes from min of ticket_expiration and proxy-token's
     * expiration.
     */
    expiration = (ticket_expiration < sub_pt->expiration) ?
        ticket_expiration : sub_pt->expiration;

    SET_TOKEN_TYPE(WA_TT_CRED);
    SET_CRED_TYPE(ct);
    SET_CRED_DATA(ticket, ticket_len);
    SET_SUBJECT(sub_pt->subject);
    SET_CREATION_TIME(creation);
    SET_EXPIRATION_TIME(expiration);

    ms = make_token_with_key(rc, &req_cred->u.st.key,
                             alist, creation,
                             (char**)&rtoken->token_data, 
                             &tlen, 1, mwk_func);
    free(ticket);
    webauth_attr_list_free(alist);
    return ms;
}

static enum mwk_status
handle_getTokensRequest(MWK_REQ_CTXT *rc, apr_xml_elem *e)
{
    apr_xml_elem *child, *tokens, *token;
    static const char *mwk_func="handle_getTokensRequest";
    const char *mid = NULL;
    char *request_token;
    MWK_REQUEST_TOKEN req_token;
    MWK_REQUESTER_CREDENTIAL req_cred;
    MWK_SUBJECT_CREDENTIAL sub_cred;
    int req_cred_parsed = 0;
    int sub_cred_parsed = 0;
    int num_tokens, i;

    MWK_RETURNED_TOKEN rtokens[MAX_TOKENS_RETURNED];

    tokens = NULL;
    request_token = NULL;
    memset(&req_cred, 0, sizeof(req_cred));
    memset(&sub_cred, 0, sizeof(sub_cred));

    /* walk through each child element in <getTokensRequest> */
    for (child = e->first_child; child; child = child->next) {
        if (strcmp(child->name, "requesterCredential") == 0) {
            if (!parse_requesterCredential(rc, child, &req_cred, 1))
                return MWK_ERROR;
            req_cred_parsed = 1;
        } else if (strcmp(child->name, "subjectCredential") == 0) {
            if (!parse_subjectCredential(rc, child, &sub_cred))
                return MWK_ERROR;
            sub_cred_parsed = 1;
        } else if (strcmp(child->name, "messageId") == 0) {
            mid = get_elem_text(rc, child, mwk_func);
            if (mid == NULL)
                return MWK_ERROR;
        } else if (strcmp(child->name, "requestToken") == 0) {
            request_token = get_elem_text(rc, child, mwk_func);
            if (request_token == NULL)
                return MWK_ERROR;
        } else if (strcmp(child->name, "tokens") == 0) {
            tokens = child;
        } else {
            unknown_element(rc, mwk_func, e->name, child->name);
            return MWK_ERROR;
        }
    }

    /* make sure we found some tokens */
    if (tokens == NULL) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "missing <tokens> in getTokensRequest",
                                 mwk_func, 1);
    }

    /* make sure we found requesterCredential */
    if (!req_cred_parsed) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                          "missing <requesterCredential> in getTokensRequest",
                          mwk_func, 1);
    }

    /* make sure sub_cred looks ok if its present */
    if (sub_cred_parsed && strcmp(sub_cred.type, "proxy") != 0) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "<subjectCredential> should be of type proxy",
                                 mwk_func, 1);
    }

    /* if req_cred is of type "service", compare command name */
    if (strcmp(req_cred.type, "service") == 0) {

        /* make sure we found requestToken */
        if (request_token == NULL) {
            return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                   "missing <requestToken>",
                                   mwk_func, 1);
        }
        /* parse request_token */
        if (!parse_request_token(rc, request_token, 
                                    &req_cred.u.st, &req_token, 0)) {
            return MWK_ERROR;
        }

        if (strcmp(req_token.cmd, "getTokensRequest") != 0) {
            return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                     "xml command in request-token "
                                     "doesn't match",
                                     mwk_func, 1);
        }
    }

    num_tokens = 0;
    /* plow through each <token> in <tokens> */
    for (token = tokens->first_child; token; token = token->next) {
        const char *tt;

        if (strcmp(token->name, "token") != 0) {
            unknown_element(rc, mwk_func, tokens->name, token->name);
            return MWK_ERROR;
        }

        if (num_tokens == MAX_TOKENS_RETURNED) {
            return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                     "too many tokens requested",
                                     mwk_func, 1);
        }

        rtokens[num_tokens].session_key = NULL;
        rtokens[num_tokens].expires = NULL;
        rtokens[num_tokens].token_data = NULL;
        rtokens[num_tokens].id = get_attr_value(rc, token, "id",
                                                   0, mwk_func);

        tt = get_attr_value(rc, token, "type", 1, mwk_func);
        if (tt == NULL)
            return MWK_ERROR;

        /* make sure we found subjectCredential if requesting
         * a token type other then "sevice".
         */
        if (strcmp(tt, "service") !=0 && !sub_cred_parsed) {
            return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                     "missing <subjectCredential> "
                                     "in getTokensRequest",
                                     mwk_func, 1);
        }

        if (strcmp(tt, "service") == 0) {
            if (!create_service_token_from_req(rc, &req_cred,
                                               &rtokens[num_tokens])) {
                return MWK_ERROR;
            }
        } else if (strcmp(tt, "id") == 0) {
            const char *at;
            apr_xml_elem *auth;

            auth = get_element(rc, token, "authenticator", 1, mwk_func);
            if (auth == NULL)
                return MWK_ERROR;
            at = get_attr_value(rc, auth, "type", 1, mwk_func);
            if (at == NULL) 
                return MWK_ERROR;

            if (!create_id_token_from_req(rc, at, &req_cred, &sub_cred,
                                          &rtokens[num_tokens])) {
                return MWK_ERROR;
            }
        } else if (strcmp(tt, "proxy") == 0) {
            apr_xml_elem *proxy_type;
            const char *pt;

            proxy_type = get_element(rc, token, "proxyType", 1, mwk_func);
            
            if (proxy_type == NULL)
                return MWK_ERROR;

            pt = get_elem_text(rc, proxy_type, mwk_func);
            if (pt == NULL) 
                return MWK_ERROR;

            if (!create_proxy_token_from_req(rc, pt, &req_cred, &sub_cred,
                                             &rtokens[num_tokens])) {
                return MWK_ERROR;
            }
        } else if (strcmp(tt, "cred") == 0) {
            if (!create_cred_token_from_req(rc, token, &req_cred, &sub_cred,
                                            &rtokens[num_tokens])) {
                return MWK_ERROR;
            }
        } else {
            char *msg = apr_psprintf(rc->r->pool, 
                                     "unknown token type: %s", tt);
            return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, msg,
                                     mwk_func, 1);
        }
        num_tokens++;
    }

    /* if we got here, we made it! */
    ap_rvputs(rc->r, "<getTokensResponse><tokens>", NULL);

    for (i = 0; i < num_tokens; i++) {
        if (rtokens[i].id != NULL) {
            ap_rprintf(rc->r, "<token id=\"%s\">",
                        apr_xml_quote_string(rc->r->pool, rtokens[i].id, 1));
        } else {
            ap_rvputs(rc->r, "<token>", NULL);
        }
        /* don't have to quote these, since they are base64'd data
           or numeric strings */
        ap_rvputs(rc->r,"<tokenData>", rtokens[i].token_data, 
                  "</tokenData>", NULL);
        if (rtokens[i].session_key) {
            ap_rvputs(rc->r,"<sessionKey>", rtokens[i].session_key,
                  "</sessionKey>", NULL);
        }
        if (rtokens[i].expires) {
            ap_rvputs(rc->r,"<expires>", rtokens[i].expires,
                      "</expires>", NULL);
        }
        ap_rvputs(rc->r, "</token>", NULL);
    }
    ap_rvputs(rc->r, "</tokens></getTokensResponse>", NULL);
    ap_rflush(rc->r);

    return MWK_OK;
}

/*
 * attempt to login. If successful, fill in both sub_cred and
 * rtokens and return MWK_OK. If unsuccessful, generate an errorResponse/log
 * and return MWK_ERROR.
 *
 * This is the point at which different types of authentication
 * could be plugged in, and the point at which we should create
 * all the different types of proxy-tokens we'll be needing at
 * login time.
 */

static enum mwk_status
mwk_do_login(MWK_REQ_CTXT *rc,
             MWK_LOGIN_TOKEN *lt,
             MWK_SUBJECT_CREDENTIAL *sub_cred,
             MWK_RETURNED_PROXY_TOKEN rtokens[],
             int *num_rtokens) 
{
    static const char*mwk_func = "mwk_do_login";
    WEBAUTH_KRB5_CTXT *ctxt;
    char *subject, *server_principal;
    int status, tgt_len, len;
    enum mwk_status ms;
    time_t tgt_expiration, creation;
    void *tgt;
    MWK_PROXY_TOKEN *pt;
    WEBAUTH_ATTR_LIST *alist;

    ms = MWK_ERROR;

    ctxt = mwk_get_webauth_krb5_ctxt(rc->r, mwk_func);
    if (ctxt == NULL) {
        /* mwk_get_webauth_krb5_ctxt already logged error */
        return set_errorResponse(rc, WA_PEC_SERVER_FAILURE, 
                                      "server failure", mwk_func, 0);
    }

    status = webauth_krb5_init_via_password(ctxt,
                                            lt->username,
                                            lt->password,
                                            rc->sconf->keytab_path,
                                            NULL,
                                            &server_principal);

    if (status == WA_ERR_LOGIN_FAILED) {
        char *msg = mwk_webauth_error_message(rc->r, status, ctxt,
                                             "login failed");


        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webkdc: %s: %s", mwk_func, msg);


        /* FIXME: we normally wouldn't log failures, would we? */
        set_errorResponse(rc, WA_PEC_LOGIN_FAILED, msg, mwk_func, 1);
        goto cleanup;
    } else if (status != WA_ERR_NONE) {
        char *msg = mwk_webauth_error_message(rc->r, status, ctxt,
                                             "webauth_krb5_init_via_password");
        set_errorResponse(rc, WA_PEC_SERVER_FAILURE, msg, mwk_func, 1);
        goto cleanup;
    } else {
        /* copy server_principal to request pool */
        char *temp = apr_pstrcat(rc->r->pool, "krb5:", server_principal, NULL);
        free(server_principal);
        server_principal = temp;
    }


    /* get subject, attempt local-name conversion */
    status = webauth_krb5_get_principal(ctxt, &subject, 1);
    if (status != WA_ERR_NONE) {
        char *msg = mwk_webauth_error_message(rc->r, status, ctxt,
                                             "webauth_krb5_get_principal");
        set_errorResponse(rc, WA_PEC_SERVER_FAILURE, msg, mwk_func, 1);
        goto cleanup;
    } else {
        char *new_subject = apr_pstrdup(rc->r->pool, subject);
        free(subject);
        subject = new_subject;
    }

    /* export TGT for webkdc-proxy-token */
    status = webauth_krb5_export_tgt(ctxt, (unsigned char**)&tgt, 
                                     &tgt_len, &tgt_expiration);
    if (status != WA_ERR_NONE) {
        char *msg = mwk_webauth_error_message(rc->r, status, ctxt,
                                              "webauth_krb5_export_tgt");
        set_errorResponse(rc, WA_PEC_SERVER_FAILURE, msg, mwk_func, 1);
        goto cleanup;
    } else {
        void *new_tgt = apr_palloc(rc->r->pool, tgt_len);
        memcpy(new_tgt, tgt, tgt_len);
        free(tgt);
        tgt = new_tgt;
    }

    /* we now have everything we need to create the webkdc-proy-token 
     * lets load up data in the sub_cred proxy token and use it
     * to create a token we'll return.
     *
     * we've already copied all this stuff into a pool, so there is no
     * need to copy again...
     */

    pt = &sub_cred->u.proxy.pt[0];

    pt->proxy_type = "krb5";
    pt->proxy_subject = server_principal;
    pt->subject = subject;
    pt->proxy_data = tgt;
    pt->proxy_data_len = tgt_len;

    /* if ProxyTopkenMaxLifetime is non-zero, use the min of it 
       and the tgt, else just use the tgt  */
    if (rc->sconf->proxy_token_max_lifetime) {
        pt->expiration = 
            (tgt_expiration < rc->sconf->proxy_token_max_lifetime) ?
            tgt_expiration : rc->sconf->proxy_token_max_lifetime;
    } else {
        pt->expiration = tgt_expiration;
    }

    time(&creation);

    alist = new_attr_list(rc, mwk_func);
    if (alist == NULL)
        goto cleanup;

    SET_TOKEN_TYPE(WA_TT_WEBKDC_PROXY);
    SET_PROXY_SUBJECT(pt->proxy_subject);
    SET_PROXY_TYPE(pt->proxy_type);
    SET_SUBJECT(pt->subject);
    SET_PROXY_DATA(tgt, tgt_len);
    SET_CREATION_TIME(creation);
    SET_EXPIRATION_TIME(pt->expiration);

    ms = make_token(rc, alist, creation,
                    (char**)&rtokens[0].token_data, &len, 1, mwk_func);
    if (ms == MWK_OK) {
        rtokens[0].type = pt->proxy_type;
        *num_rtokens = 1;
        sub_cred->u.proxy.num_proxy_tokens = 1;
        /* make sure we fill in type! */
        sub_cred->type = "proxy";
    }

    webauth_attr_list_free(alist);
    
 cleanup:        

    webauth_krb5_free(ctxt);
    return ms;
}

static enum mwk_status
handle_requestTokenRequest(MWK_REQ_CTXT *rc, apr_xml_elem *e)
{
    apr_xml_elem *child;
    static const char *mwk_func="handle_requestTokenRequest";
    char *request_token;
    MWK_REQUESTER_CREDENTIAL req_cred;
    MWK_SUBJECT_CREDENTIAL parsed_sub_cred, login_sub_cred, *sub_cred;
    enum mwk_status ms;
    MWK_REQUEST_TOKEN req_token;
    int req_cred_parsed = 0;
    int sub_cred_parsed = 0;
    int num_tokens, i, did_login;
    int login_ec;
    const char *login_em;

    MWK_RETURNED_TOKEN rtoken;
    MWK_RETURNED_PROXY_TOKEN rptokens[MAX_PROXY_TOKENS_RETURNED];

    login_ec = 0;
    request_token = NULL;
    memset(&req_cred, 0, sizeof(req_cred));
    memset(&sub_cred, 0, sizeof(sub_cred));
    memset(&req_token, 0, sizeof(req_token));
    memset(&rtoken, 0, sizeof(rtoken));

    /* walk through each child element in <requestTokenRequest> */
    for (child = e->first_child; child; child = child->next) {
        if (strcmp(child->name, "requesterCredential") == 0) {
            if (!parse_requesterCredential(rc, child, &req_cred, 0))
                return MWK_ERROR;
            req_cred_parsed = 1;
        } else if (strcmp(child->name, "subjectCredential") == 0) {
            if (!parse_subjectCredential(rc, child, &parsed_sub_cred))
                return MWK_ERROR;
            sub_cred_parsed = 1;
        } else if (strcmp(child->name, "requestToken") == 0) {
            request_token = get_elem_text(rc, child, mwk_func);
            if (request_token == NULL)
                return MWK_ERROR;
        } else {
            unknown_element(rc, mwk_func, e->name, child->name);
            return MWK_ERROR;
        }
    }

    /* make sure we found requesterCredential */
    if (!req_cred_parsed) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "missing <requesterCredential>",
                                 mwk_func, 1);
    }

    /* make sure we found subjectCredentials */
    if (!sub_cred_parsed) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "missing <subjectCredential>",
                                 mwk_func, 1);
    }

    /* make sure req_cred is of type "service" */
    if (strcmp(req_cred.type, "service") != 0) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "must use <requesterCredential> "
                                 "of type 'service'",
                                 mwk_func, 1);
    }

    /* make sure we found requestToken */
    if (request_token == NULL) {
        return set_errorResponse(rc, WA_PEC_INVALID_REQUEST, 
                                 "missing <requestToken>",
                                 mwk_func, 1);
    }

    if (!parse_request_token(rc, request_token, 
                             &req_cred.u.st, &req_token, 0)) {
        return MWK_ERROR;
    }

    /*
     * if we have a login-token, attempt to login with it,
     * and if that succeeds, we'll get a new MWK_SUBJECT_CREDENTIAL
     * to pass around, and new proxy-tokens to set.
     *
     */
    if (strcmp(parsed_sub_cred.type, "login") == 0) {
        if (!mwk_do_login(rc, &parsed_sub_cred.u.lt, 
                          &login_sub_cred, rptokens, &num_tokens)) {
            if (rc->error_code == WA_PEC_LOGIN_FAILED) {
                login_ec = rc->error_code;
                login_em = rc->error_message;
                rc->error_code = 0;
                rc->error_message = NULL;
                goto send_response;
            } else {
                return MWK_ERROR;
            }
        }
        sub_cred = &login_sub_cred;
        did_login = 1;
    } else {
        sub_cred = &parsed_sub_cred;
        did_login = 0;
    }

    /* lets see if they requested forced-authentication, if so 
       and we didn't just login, then we need
       to return an error that will cause the web front-end to 
       prompt for a username/password */
    if ((ap_strstr(req_token.request_options, "fa") != 0) &&
        !did_login) {
        const char *msg = "forced authentication, need to login";
        login_ec = WA_PEC_LOGIN_FORCED;
        login_em = msg;
        goto send_response;
    }

    /* now examine req_token to see what they asked for */
    
    if (strcmp(req_token.requested_token_type, "id") == 0) {
        ms = create_id_token_from_req(rc, req_token.u.subject_auth_type,
                                      &req_cred, sub_cred, &rtoken);
    } else if (strcmp(req_token.requested_token_type, "proxy") == 0) {
        ms = create_proxy_token_from_req(rc, req_token.u.proxy_type,
                                         &req_cred, sub_cred, &rtoken);
    } else {
        char *msg = apr_psprintf(rc->r->pool, 
                                 "unsupported requested-token-type: %s",
                                 req_token.requested_token_type);
        set_errorResponse(rc, WA_PEC_INVALID_REQUEST, msg,
                               mwk_func, 1);
        return MWK_ERROR;
    }

    if (ms != MWK_OK) {
        switch (rc->error_code) {
            case WA_PEC_PROXY_TOKEN_REQUIRED:
                login_ec = rc->error_code;
                login_em = rc->error_message;
                goto send_response;
            case WA_PEC_UNAUTHORIZED:
                /* for some error codes we want to return 
                   an error token as the requestedToken */
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                             "mod_webkdc: %s: %s (%d)", rc->mwk_func, 
                             rc->error_message, rc->error_code);

                ms = create_error_token_from_req(rc, 
                                                 rc->error_code,
                                                 rc->error_message,
                                                 &req_cred,
                                                 &rtoken);
                if (ms == MWK_OK)
                    goto send_response;
                else 
                    return MWK_ERROR;                    
            default:
                return MWK_ERROR;
        }
    }


 send_response:

    ap_rvputs(rc->r, "<requestTokenResponse>", NULL);

    if (login_ec) {
        ap_rprintf(rc->r, "<loginErrorCode>%d</loginErrorCode>", login_ec);
        ap_rprintf(rc->r, "<loginErrorMessage>%s</loginErrorMessage>", 
                   apr_xml_quote_string(rc->r->pool, login_em, 0));
    }

    if (num_tokens) {
        ap_rvputs(rc->r, "<proxyTokens>", NULL);
        for (i = 0; i < num_tokens; i++) {
            ap_rvputs(rc->r, "<proxyToken type='", rptokens[i].type,"'>", 
                      /* don't have to quote since base64'd data */
                      rptokens[i].token_data,
                      "</proxyToken>",
                      NULL);
        }
        ap_rvputs(rc->r, "</proxyTokens>", NULL);        
    }
    /* put out return-url */
    ap_rvputs(rc->r,"<returnUrl>",
              apr_xml_quote_string(rc->r->pool, req_token.return_url, 1),
              "</returnUrl>", NULL);

    /* requesterSubject */
    ap_rvputs(rc->r,
              "<requesterSubject>",
              apr_xml_quote_string(rc->r->pool, req_cred.subject, 1),
              "</requesterSubject>", NULL);

    /* requestedToken, don't need to quote */
    if (rtoken.token_data)
        ap_rvputs(rc->r,
                  "<requestedToken>", 
                  rtoken.token_data,
                  "</requestedToken>", 
                  NULL);

    if (ap_strstr(req_token.request_options, "lc")) {
        MWK_RETURNED_TOKEN lc_token;
        enum mwk_status ms;
        memset(&lc_token, 0, sizeof(lc_token));
        ms = create_error_token_from_req(rc, 
                                         WA_PEC_LOGIN_CANCELED,
                                         "user canceled login", 
                                         &req_cred,
                                         &lc_token);
        if (ms == MWK_OK) 
            ap_rvputs(rc->r,
                      "<loginCanceledToken>", 
                      lc_token.token_data, 
                      "</loginCanceledToken>", 
                      NULL);
    }

    /* appState, need to base64-encode */
    if (req_token.app_state_len) {
        char *out_state = (char*) 
            apr_palloc(rc->r->pool, 
                       apr_base64_encode_len(req_token.app_state_len));
        apr_base64_encode(out_state, req_token.app_state,
                          req_token.app_state_len);
        /*  don't need to quote */
        ap_rvputs(rc->r,
                  "<appState>", out_state , "</appState>", 
                  NULL);
    }
    ap_rvputs(rc->r, "</requestTokenResponse>", NULL);
    ap_rflush(rc->r);

    return MWK_OK;
}

static int
parse_request(MWK_REQ_CTXT *rc)
{
    int s, num_read;
    char buff[8192];
    apr_xml_parser *xp;
    apr_xml_doc *xd;
    apr_status_t astatus;
    const char *mwk_func = "parse_request";

    xp = apr_xml_parser_create(rc->r->pool);
    if (xp == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                     "mod_webkdc: %s: "
                     "apr_xml_parser_create failed", mwk_func);
        set_errorResponse(rc, WA_PEC_SERVER_FAILURE, 
                          "server failure", mwk_func, 0);
        generate_errorResponse(rc);
        return OK;
    }

    s = ap_setup_client_block(rc->r, REQUEST_CHUNKED_DECHUNK);
    if (s!= OK)
        return s;

    astatus = APR_SUCCESS;
    num_read = 0;

    while (astatus == APR_SUCCESS &&
           ((num_read = ap_get_client_block(rc->r, buff, sizeof(buff))) > 0)) {
        astatus = apr_xml_parser_feed(xp, buff, num_read);
    }

    if (num_read == 0 && astatus == APR_SUCCESS)
        astatus = apr_xml_parser_done(xp, &xd);

    if ((num_read < 0) || astatus != APR_SUCCESS) {
        if (astatus != APR_SUCCESS) {
            char errbuff[1024] = "";
            apr_xml_parser_geterror(xp, errbuff, sizeof(errbuff));
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server, 
                         "mod_webkdc: %s: "
                         "apr_xml_parser_feed failed: %s (%d)", 
                         mwk_func,
                         errbuff,
                         astatus);
            set_errorResponse(rc, WA_PEC_INVALID_REQUEST, errbuff,
                              mwk_func, 0);
            generate_errorResponse(rc);
            return OK;
        } else {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                       "mod_webkdc: %s: ap_get_client_block error", mwk_func);
            set_errorResponse(rc, WA_PEC_INVALID_REQUEST,
                              "read error while parsing", mwk_func, 0);
            generate_errorResponse(rc);
            return OK;
        }
    }

    if (strcmp(xd->root->name, "getTokensRequest") == 0) {
        if (!handle_getTokensRequest(rc, xd->root))
            generate_errorResponse(rc);
    } else if (strcmp(xd->root->name, "requestTokenRequest") == 0) {
        if (!handle_requestTokenRequest(rc, xd->root))
            generate_errorResponse(rc);
    } else {
        char *m = apr_psprintf(rc->r->pool, "invalid command: %s", 
                               xd->root->name);
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, rc->r->server,
                     "mod_webkdc: %s: %s", mwk_func, m);
        set_errorResponse(rc, WA_PEC_INVALID_REQUEST, m, mwk_func, 0);
        generate_errorResponse(rc);
        return OK;        
    }
    return OK;
}

/* The content handler */
static int 
handler_hook(request_rec *r)
{
    MWK_REQ_CTXT rc;
    const char *req_content_type;

    memset(&rc, 0, sizeof(rc));

    rc.r = r;
    rc.sconf = (MWK_SCONF*)ap_get_module_config(r->server->module_config,
                                                &webkdc_module);

    if (strcmp(r->handler, "webkdc")) {
        return DECLINED;
    }

    req_content_type = apr_table_get(r->headers_in, "content-type");

    if (!req_content_type || strcmp(req_content_type, "text/xml")) {
        return HTTP_BAD_REQUEST;
    }

    if (r->method_number != M_POST) {
        return HTTP_METHOD_NOT_ALLOWED;
    }

    ap_set_content_type(r, "text/xml");
    return parse_request(&rc);
}

static int 
die(const char *message, server_rec *s)
{
    if (s) {
        ap_log_error(APLOG_MARK, APLOG_ERR, 0, s,
                     "mod_webkdc: fatal error: %s", message);
    }
    printf("mod_webkdc: fatal error: %s\n", message);
    exit(1);
}

/*
 * called after config has been loaded in parent process
 */
static int
mod_webkdc_init(apr_pool_t *pconf, apr_pool_t *plog,
                apr_pool_t *ptemp, server_rec *s)
{
    MWK_SCONF *sconf;
    int status;
    WEBAUTH_KEYRING *ring;

    sconf = (MWK_SCONF*)ap_get_module_config(s->module_config,
                                             &webkdc_module);

    ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "mod_webkdc: initializing");

#define CHECK_DIR(field,dir,v) if (sconf->field == v) \
             die(apr_psprintf(ptemp, "directive %s must be set", dir), s)

    CHECK_DIR(keyring_path, CD_Keyring, NULL);
    CHECK_DIR(keytab_path, CD_Keytab, NULL);
    CHECK_DIR(service_token_lifetime, CD_ServiceTokenLifetime, 0);

#undef CHECK_DIR

    /* attempt to open keyring */
    status = webauth_keyring_read_file(sconf->keyring_path, &ring);
    if (status != WA_ERR_NONE) {
        die(apr_psprintf(ptemp, 
                 "mod_webkdc: webauth_keyring_read_file(%s) failed: %s (%d)",
                         sconf->keyring_path, webauth_error_message(status), 
                         status), s);
    } else {
        /* close it, and open it in child */
        webauth_keyring_free(ring);
    }

    ap_add_version_component(pconf, WEBKDC_VERSION);

    ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "mod_webkdc: initialized");

    return OK;
}

/*
 * called once per-child
 */
static void
mod_webkdc_child_init(apr_pool_t *p, server_rec *s)
{
    /* initialize mutexes */
    mwk_init_mutexes(s);
}

/*
**
**  per-server configuration structure handling
**
*/

static void *
config_server_create(apr_pool_t *p, server_rec *s)
{
    MWK_SCONF *sconf;

    sconf = (MWK_SCONF*)apr_pcalloc(p, sizeof(MWK_SCONF));

    /* init defaults */
    sconf->token_max_ttl = DF_TokenMaxTTL;
    return (void *)sconf;
}

#define MERGE_PTR(field) \
    conf->field = (oconf->field != NULL) ? oconf->field : bconf->field

#define MERGE_INT(field) \
    conf->field = oconf->field ? oconf->field : bconf->field

static void *
config_server_merge(apr_pool_t *p, void *basev, void *overv)
{
    MWK_SCONF *conf, *bconf, *oconf;

    conf = (MWK_SCONF*) apr_pcalloc(p, sizeof(MWK_SCONF));
    bconf = (MWK_SCONF*) basev;
    oconf = (MWK_SCONF*) overv;

    conf->token_max_ttl = oconf->token_max_ttl_ex ?
        oconf->token_max_ttl : bconf->token_max_ttl;

    conf->debug = oconf->debug_ex ? oconf->debug : bconf->debug;

    MERGE_PTR(keyring_path);
    MERGE_PTR(keytab_path);
    MERGE_INT(proxy_token_max_lifetime);
    MERGE_INT(service_token_lifetime);
    return (void *)conf;
}

#undef MERGE_PTR
#undef MERGE_INT

static int
seconds(const char *value, char **error_str)
{
    char temp[32];
    int mult, len;
    
    len = strlen(value);
    if (len > (sizeof(temp)-1)) {
        *error_str = "error: value too long!";
        return 0;
    }

    strcpy(temp, value);

    switch(temp[len-1]) {
        case 's': 
            mult = 1;
            break;
        case 'm':
            mult = 60;
            break;
        case 'h': 
            mult = 60*60; 
            break;
        case 'd': 
            mult = 60*60*24; 
            break;
        case 'w': 
            mult = 60*60*24*7; 
            break;
        default:
            *error_str = "error: value too long!";
            return 0;
            break;
    }
    
    temp[len-1] = '\0';
    return atoi(temp) * mult;
}

static const char *
cfg_str(cmd_parms *cmd, void *mconf, const char *arg)
{
    int e = (int)cmd->info;
    char *error_str = NULL;

    MWK_SCONF *sconf = (MWK_SCONF *)
        ap_get_module_config(cmd->server->module_config, &webkdc_module);
    
    switch (e) {
        /* server configs */
        case E_Keyring:
            sconf->keyring_path = ap_server_root_relative(cmd->pool, arg);
            break;
        case E_Keytab:
            sconf->keytab_path = ap_server_root_relative(cmd->pool, arg);
            break;
        case E_ProxyTokenMaxLifetime:
            sconf->proxy_token_max_lifetime = seconds(arg, &error_str);
            break;
        case E_TokenMaxTTL:
            sconf->token_max_ttl = seconds(arg, &error_str);
            sconf->token_max_ttl_ex = 1;
            break;
        case E_ServiceTokenLifetime:
            sconf->service_token_lifetime = seconds(arg, &error_str);
            break;
        default:
            error_str = 
                apr_psprintf(cmd->pool,
                             "Invalid value cmd->info(%d) for directive %s",
                             e,
                             cmd->directive->directive);
            break;
    }
    return error_str;
}

static const char *
cfg_flag(cmd_parms *cmd, void *mconfig, int flag)
{
    int e = (int)cmd->info;
    char *error_str = NULL;

    MWK_SCONF *sconf = (MWK_SCONF *)
        ap_get_module_config(cmd->server->module_config, &webkdc_module);
    
    switch (e) {
        /* server configs */
        case E_Debug:
            sconf->debug = flag;
            sconf->debug_ex = 1;
            break;
        default:
            error_str = 
                apr_psprintf(cmd->pool,
                             "Invalid value cmd->info(%d) for directive %s",
                             e,
                             cmd->directive->directive);
            break;

    }
    return error_str;
}


#define SSTR(dir,mconfig,help) \
  {dir, (cmd_func)cfg_str,(void*)mconfig, RSRC_CONF, TAKE1, help}

#define SFLAG(dir,mconfig,help) \
  {dir, (cmd_func)cfg_flag,(void*)mconfig, RSRC_CONF, FLAG, help}

static const command_rec cmds[] = {
    /* server/vhost */
    SSTR(CD_Keyring, E_Keyring, CM_Keyring),
    SSTR(CD_Keytab, E_Keytab,  CM_Keytab),
    SFLAG(CD_Debug, E_Debug, CM_Debug),
    SSTR(CD_TokenMaxTTL, E_TokenMaxTTL, CM_TokenMaxTTL),
    SSTR(CD_ProxyTokenMaxLifetime, E_ProxyTokenMaxLifetime, 
         CM_ProxyTokenMaxLifetime),
    SSTR(CD_ServiceTokenLifetime, E_ServiceTokenLifetime, 
         CM_ServiceTokenLifetime),
    { NULL }
};

#undef SSTR
#undef SFLAG

static void 
register_hooks(apr_pool_t *p)
{
    ap_hook_post_config(mod_webkdc_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(mod_webkdc_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(handler_hook, NULL, NULL, APR_HOOK_MIDDLE);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA webkdc_module = {
    STANDARD20_MODULE_STUFF, 
    NULL,                  /* create per-dir    config structures */
    NULL,                  /* merge  per-dir    config structures */
    config_server_create,  /* create per-server config structures */
    config_server_merge,   /* merge  per-server config structures */
    cmds,                  /* table of config file commands       */
    register_hooks         /* register hooks                      */
};

