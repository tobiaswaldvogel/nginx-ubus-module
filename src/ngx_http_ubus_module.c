
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <ubus_utility.h>

static void* ngx_http_ubus_create_loc_conf(ngx_conf_t *cf);

static char* ngx_http_ubus_merge_loc_conf(ngx_conf_t *cf,
		void *parent, void *child);

static char *ngx_http_ubus(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

typedef struct {
		ngx_str_t socket_path;
		ngx_flag_t cors;
		ngx_uint_t script_timeout;
		ngx_flag_t noauth;
		ngx_flag_t enable;
		ngx_uint_t parallel_req;
} ngx_http_ubus_loc_conf_t;

static ngx_command_t  ngx_http_ubus_commands[] = {
		{ ngx_string("ubus_interpreter"),
			NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
			ngx_http_ubus,
			NGX_HTTP_LOC_CONF_OFFSET,
			0,
			NULL },

		{ ngx_string("ubus_socket_path"),
			NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
			ngx_conf_set_str_slot,
			NGX_HTTP_LOC_CONF_OFFSET,
			offsetof(ngx_http_ubus_loc_conf_t, socket_path),
			NULL },

		{ ngx_string("ubus_cors"),
			NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
			ngx_conf_set_flag_slot,
			NGX_HTTP_LOC_CONF_OFFSET,
			offsetof(ngx_http_ubus_loc_conf_t, cors),
			NULL },

		{ ngx_string("ubus_script_timeout"),
			NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
			ngx_conf_set_num_slot,
			NGX_HTTP_LOC_CONF_OFFSET,
			offsetof(ngx_http_ubus_loc_conf_t, script_timeout),
			NULL },

		{ ngx_string("ubus_noauth"),
			NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
			ngx_conf_set_flag_slot,
			NGX_HTTP_LOC_CONF_OFFSET,
			offsetof(ngx_http_ubus_loc_conf_t, noauth),
			NULL },

		{ ngx_string("ubus_parallel_req"),
			NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
			ngx_conf_set_num_slot,
			NGX_HTTP_LOC_CONF_OFFSET,
			offsetof(ngx_http_ubus_loc_conf_t, parallel_req),
			NULL },

			ngx_null_command
};

static ngx_http_module_t  ngx_http_ubus_module_ctx = {
		NULL,   /* preconfiguration */
		NULL,  /* postconfiguration */

		NULL,                          /* create main configuration */
		NULL,                          /* init main configuration */

		NULL,                          /* create server configuration */
		NULL,                          /* merge server configuration */

		ngx_http_ubus_create_loc_conf,  /* create location configuration */
		ngx_http_ubus_merge_loc_conf /* merge location configuration */
};


ngx_module_t  ngx_http_ubus_module = {
		NGX_MODULE_V1,
		&ngx_http_ubus_module_ctx, /* module context */
		ngx_http_ubus_commands,   /* module directives */
		NGX_HTTP_MODULE,               /* module type */
		NULL,                          /* init master */
		NULL,                          /* init module */
		NULL,                          /* init process */
		NULL,                          /* init thread */
		NULL,                          /* exit thread */
		NULL,                          /* exit process */
		NULL,                          /* exit master */
		NGX_MODULE_V1_PADDING
};

struct cors_data {
	char* ORIGIN;
	char* ACCESS_CONTROL_REQUEST_METHOD;
	char* ACCESS_CONTROL_REQUEST_HEADERS;
};

static void ubus_single_error(request_ctx_t *request, enum rpc_status type, bool array);
static ngx_int_t ngx_http_ubus_send_body(request_ctx_t *request);
static ngx_int_t append_to_output_chain(request_ctx_t *request,  const char* str);
static void setup_ubus_ctx_t(ubus_ctx_t *ctx, request_ctx_t *request, json_object *obj);
static void free_ubus_ctx_t(ubus_ctx_t *ctx,ngx_http_request_t *r);

static ngx_int_t set_custom_headers_out(ngx_http_request_t *r, const char *key_str, const char *value_str) {
	ngx_table_elt_t   *h;
	ngx_str_t key;
	ngx_str_t value;

	char * tmp;
	int len;

	len = strlen(key_str);
	tmp = ngx_palloc(r->pool,len + 1);
	ngx_memcpy(tmp,key_str,len);

	key.data = tmp;
	key.len = len;

	len = strlen(value_str);
	tmp = ngx_palloc(r->pool,len + 1);
	ngx_memcpy(tmp,value_str,len);

	value.data = tmp;
	value.len = len;

	h = ngx_list_push(&r->headers_out.headers);
	if (h == NULL) {
		return NGX_ERROR;
	}

	h->key = key;
	h->value = value;
	h->hash = 1;

	return NGX_OK;
}

static void parse_cors_from_header(ngx_http_request_t *r, struct cors_data *cors) {
	ngx_list_part_t            *part;
	ngx_table_elt_t            *h;
	ngx_uint_t                  i;

	ngx_uint_t found_count = 0;

	part = &r->headers_in.headers.part;
	h = part->elts;

	for (i = 0; /* void */ ; i++) {
		if ( found_count == 3 )
			break;

		if (i >= part->nelts) {
			if (part->next == NULL) {
				break;
			}

			part = part->next;
			h = part->elts;
			i = 0;
		}

		if (ngx_strcmp("origin", h[i].key.data)) {
			cors->ORIGIN = h[i].key.data;
			found_count++;
		}
		else if (ngx_strcmp("access-control-request-method", h[i].key.data)) {
			cors->ACCESS_CONTROL_REQUEST_METHOD = h[i].key.data;
			found_count++;
		}
		else if (ngx_strcmp("access-control-request-headers", h[i].key.data)) {
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ok3");
			cors->ACCESS_CONTROL_REQUEST_HEADERS = h[i].key.data;
			found_count++;
		}

	}
}

static void ubus_add_cors_headers(ngx_http_request_t *r)
{
	struct cors_data *cors;

	cors = ngx_pcalloc(r->pool,sizeof(struct cors_data));
	parse_cors_from_header(r,cors);

	char* req;

	if (!cors->ORIGIN)
		return;

	if (cors->ACCESS_CONTROL_REQUEST_METHOD)
	{
		char *req = cors->ACCESS_CONTROL_REQUEST_METHOD;
		if (strcmp(req, "POST") && strcmp(req, "OPTIONS"))
			return;
	}

	set_custom_headers_out(r,"Access-Control-Allow-Origin",cors->ORIGIN);

	if (cors->ACCESS_CONTROL_REQUEST_HEADERS)
		set_custom_headers_out(r,"Access-Control-Allow-Headers",cors->ACCESS_CONTROL_REQUEST_HEADERS);

	set_custom_headers_out(r,"Access-Control-Allow-Methods","POST, OPTIONS");
	set_custom_headers_out(r,"Access-Control-Allow-Credentials","true");

	ngx_pfree(r->pool,cors);
}

static ngx_int_t ngx_http_ubus_send_header(
	ngx_http_request_t *r, ngx_http_ubus_loc_conf_t  *cglcf, ngx_int_t status, ngx_int_t post_len)
{
	r->headers_out.status = status;
	r->headers_out.content_type.len = sizeof("application/json") - 1;
	r->headers_out.content_type.data = (u_char *) "application/json";
	r->headers_out.content_length_n = post_len;

	if (cglcf->cors)
		ubus_add_cors_headers(r);

	return ngx_http_send_header(r);
	
}

static void ubus_single_error(request_ctx_t *request, enum rpc_status type, bool array)
{
	void *c;
	char *str;

	struct blob_buf* buf = ngx_pcalloc(request->r->pool,sizeof(struct blob_buf));

	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(request->r, ngx_http_ubus_module);

	ngx_log_error(NGX_LOG_ERR, request->r->connection->log, 0,
                      "Request generated error: %s",json_errors[type].msg);

	request->res_len = 0;
	ubus_close_fds(request->ubus_ctx);

	static struct dispatch_ubus du;

	blob_buf_init(buf, 0);

	ubus_init_response(buf,&du);

	c = blobmsg_open_table(buf, "error");
	blobmsg_add_u32(buf, "code", json_errors[type].code);
	blobmsg_add_string(buf, "message", json_errors[type].msg);
	blobmsg_close_table(buf, c);

	str = blobmsg_format_json(buf->head, true);
	append_to_output_chain(request,str);

	ngx_pfree(request->r->pool,buf);

	ngx_http_ubus_send_header(request->r,cglcf,NGX_HTTP_OK,request->res_len);
	ngx_http_ubus_send_body(request);
}

static ngx_int_t append_to_output_chain(request_ctx_t *request,  const char* str)
{
	ngx_int_t len = strlen(str);

	char* data = ngx_pcalloc(request->r->pool, len + 1);
	ngx_memcpy(data,str,len);

	ngx_buf_t *b = ngx_pcalloc(request->r->pool, sizeof(ngx_buf_t));
	b->pos = data;
	b->last = data + len;
	b->memory = 1;
	request->res_len += len;

	if (!request->out_chain) {
			request->out_chain = (ngx_chain_t *) ngx_palloc(request->r->pool, sizeof(ngx_chain_t*));
			request->out_chain->buf = b;
			request->out_chain->next = NULL;
			request->out_chain_start = request->out_chain;
	} else {
			ngx_chain_t* out_aux = (ngx_chain_t *) ngx_palloc(request->r->pool, sizeof(ngx_chain_t*));
			out_aux->buf = b;
			out_aux->next = NULL;
			request->out_chain->next = out_aux;
			request->out_chain = out_aux;
	}
}

static void setup_ubus_ctx_t(ubus_ctx_t *ctx, request_ctx_t *request, json_object *obj)
{
	ctx->ubus = ngx_pcalloc(request->r->pool,sizeof(struct dispatch_ubus));
	ctx->buf = ngx_pcalloc(request->r->pool,sizeof(struct blob_buf));
	ctx->request = request;
	ctx->obj = obj;
	ctx->ubus->jsobj = NULL;
	ctx->ubus->jstok = json_tokener_new();
}

static void free_ubus_ctx_t(ubus_ctx_t *ctx, ngx_http_request_t *r)
{
	if (ctx->ubus->jsobj)
		free(ctx->ubus->jsobj);
	if (ctx->ubus->jstok)
		free(ctx->ubus->jstok);
	ngx_pfree(r->pool,ctx->ubus);
	ngx_pfree(r->pool,ctx->buf);
	ngx_pfree(r->pool,ctx->obj);
	ngx_pfree(r->pool,ctx);
}

static ngx_int_t ngx_http_ubus_send_body(request_ctx_t *request)
{
	request->out_chain->buf->last_buf = 1;

	return ngx_http_output_filter(request->r, request->out_chain_start);
}

static bool ubus_allowed(struct ubus_context *ctx, ngx_int_t script_timeout, const char *sid, const char *obj, const char *fun)
{
	uint32_t id;
	bool allow = false;
	static struct blob_buf req;

	if (ubus_lookup_id(ctx, "session", &id))
		return false;

	blob_buf_init(&req, 0);
	blobmsg_add_string(&req, "ubus_rpc_session", sid);
	blobmsg_add_string(&req, "object", obj);
	blobmsg_add_string(&req, "function", fun);
	
	ubus_invoke(ctx, id, "access", req.head, ubus_allowed_cb, &allow, script_timeout * 500);

	return allow;
}

static enum rpc_status ubus_send_request(request_ctx_t *request, ubus_ctx_t *ctx, const char *sid, struct blob_attr *args)
{
	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(request->r, ngx_http_ubus_module);

	struct dispatch_ubus *du = ctx->ubus;
	struct blob_attr *cur;
	static struct blob_buf req;
	int ret, rem;

	char *str;

	blob_buf_init(&req, 0);

	ubus_init_response(ctx->buf,du);

	blobmsg_for_each_attr(cur, args, rem) {
		if (!strcmp(blobmsg_name(cur), "ubus_rpc_session")) {
			return ERROR_PARAMS;
		}
		blobmsg_add_blob(&req, cur);
	}

	blobmsg_add_string(&req, "ubus_rpc_session", sid);

	blob_buf_init(&du->buf, 0);
	memset(&du->req, 0, sizeof(du->req));

	if (ctx->array)
		sem_wait(request->sem);

	ubus_invoke(request->ubus_ctx, du->obj, du->func, req.head, ubus_request_cb, ctx, cglcf->script_timeout * 1000);

	if (ctx->array)
		sem_post(request->sem);

	str = blobmsg_format_json(ctx->buf->head, true);

	if (ctx->array) {

		ctx->request->array_res[ctx->index] = str;
		
	} else {
		append_to_output_chain(request,str);
		free(str);
	}

	return REQUEST_OK;
}

static enum rpc_status ubus_send_list(request_ctx_t *request, ubus_ctx_t *ctx, struct blob_attr *params)
{
	struct blob_attr *cur, *dup;

	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(request->r, ngx_http_ubus_module);

	struct dispatch_ubus *du = ctx->ubus;

	struct list_data data = { .buf = &du->buf, .verbose = false };
	void *r;
	int rem;

	char *str;

	blob_buf_init(data.buf, 0);

	ubus_init_response(ctx->buf,du);

	if (!params || blob_id(params) != BLOBMSG_TYPE_ARRAY) {
		r = blobmsg_open_array(data.buf, "result");

		if (ctx->array)
			sem_wait(request->sem);

		ubus_lookup(request->ubus_ctx, NULL, ubus_list_cb, &data);

		if (ctx->array)
			sem_post(request->sem);

		blobmsg_close_array(data.buf, r);
	}
	else {
		r = blobmsg_open_table(data.buf, "result");
		dup = blob_memdup(params);
		if (dup)
		{
			rem = blobmsg_data_len(dup);
			data.verbose = true;
			__blob_for_each_attr(cur, blobmsg_data(dup), rem)

			if (ctx->array)
				sem_wait(request->sem);

				ubus_lookup(request->ubus_ctx, blobmsg_data(cur), ubus_list_cb, &data);

			if (ctx->array)
				sem_post(request->sem);

			free(dup);
		}
		blobmsg_close_table(data.buf, r);
	}

	blobmsg_add_blob(ctx->buf, blob_data(data.buf->head));

	str = blobmsg_format_json(ctx->buf->head, true);

	if (ctx->array) {

		ctx->request->array_res[ctx->index] = str;

	} else {
		append_to_output_chain(request,str);
		free(str);
	}

	return REQUEST_OK;
}

static enum rpc_status ubus_process_object(ubus_ctx_t *ctx)
{

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
        "Start processing json object",concurrent);

	enum rpc_status rc = REQUEST_OK;
	enum rpc_status *res;
	bool array = ctx->array;
	request_ctx_t *request = ctx->request;
	
	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(request->r, ngx_http_ubus_module);

	struct dispatch_ubus *du = ctx->ubus;
	struct rpc_data data = {};
	enum rpc_status err = ERROR_PARSE;

	if (json_object_get_type(ctx->obj) != json_type_object)
		goto error;

	du->jsobj_cur = ctx->obj;
	blob_buf_init(ctx->buf, 0);
	if (!blobmsg_add_object(ctx->buf, ctx->obj))
		goto error;

	if (!parse_json_rpc(&data, ctx->buf->head))
		goto error;

	if (!strcmp(data.method, "call")) {
		if (!data.sid || !data.object || !data.function || !data.data)
			goto error;

		du->func = data.function;

		if (ctx->array)
			sem_wait(ctx->request->sem);

		if (ubus_lookup_id(ctx->request->ubus_ctx, data.object, &du->obj)) {
			err = ERROR_OBJECT;
			goto error;
		}

		if (ctx->array)
			sem_post(ctx->request->sem);

		if (ctx->array)
			sem_wait(ctx->request->sem);

		if (!cglcf->noauth && !ubus_allowed(ctx->request->ubus_ctx, cglcf->script_timeout, data.sid, data.object, data.function)) {
			err = ERROR_ACCESS;
			goto error;
		}

		if (ctx->array)
			sem_post(ctx->request->sem);

		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
        	"Start processing call request",concurrent);

		rc = ubus_send_request(request, ctx, data.sid, data.data);
		goto out;
	}
	else if (!strcmp(data.method, "list")) {
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
        	"Start processing list request",concurrent);
		rc = ubus_send_list(request, ctx, data.params);
		goto out;
	}
	else {
		err = ERROR_METHOD;
		goto error;
	}

error:
	if (ctx->array)
		sem_post(ctx->request->sem);
	rc = err;
out:
	if (data.params)
		free(data.params);

	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
        "Json object processed correctly",concurrent);

	free_ubus_ctx_t(ctx,request->r);

	if (array) {
		res = ngx_pcalloc(request->r->pool,sizeof(enum rpc_status));
		ngx_memcpy(res,&rc,sizeof(enum rpc_status));
		pthread_exit(res);
	}

	return rc;
}

static ngx_int_t ubus_process_array(request_ctx_t *request, json_object *obj)
{
	ngx_http_ubus_loc_conf_t  *cglcf;
	ngx_int_t rc = NGX_OK;
	cglcf = ngx_http_get_module_loc_conf(request->r, ngx_http_ubus_module);

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
               "Start processing array json object");

	ubus_ctx_t *ctx;
	sem_t *sem = ngx_pcalloc(request->r->pool,sizeof(sem_t));
	sem_init(sem, 0, 1);

	request->sem = sem;

	int len = json_object_array_length(obj);

	int processed = 0;
	int concurrent;
	int concurrent_thread = cglcf->parallel_req;
	int threads_spawned;

	enum rpc_status** res = ngx_pcalloc(request->r->pool,concurrent_thread * sizeof(enum rpc_status*));
	enum rpc_status err;

	pthread_t* threads = ngx_pcalloc(request->r->pool,concurrent_thread * sizeof(pthread_t));
	request->array_res = ngx_pcalloc(request->r->pool, len * sizeof(char *));

	while ( processed < len ) {

		threads_spawned = 0;

		for (concurrent = 0; concurrent < concurrent_thread && processed < len; concurrent++) {
			struct json_object *obj_tmp = json_object_array_get_idx(obj, processed);

			ngx_log_debug2(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
               "Spawning thread %d to process request %d",concurrent,processed);

			ctx = ngx_pcalloc(request->r->pool,sizeof(ubus_ctx_t));

			setup_ubus_ctx_t(ctx,request,obj_tmp);

			ctx->array = true;
			ctx->index = processed;

			pthread_create(&threads[concurrent],NULL,(void *) ubus_process_object,ctx);
			threads_spawned++;
			processed++;
		}

		for (concurrent = 0 ; concurrent < threads_spawned; concurrent++ ) {
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
               "Waiting thread %d to finish",concurrent);
			pthread_join(threads[concurrent], (void**) &res[concurrent]);

			if (*res[concurrent] != REQUEST_OK)
				err = *res[concurrent];

		}

		if (err)
			break;
	}

	ngx_pfree(request->r->pool,threads);
	ngx_pfree(request->r->pool,res);

	if (err) {

		request->out_chain = NULL;
		ubus_single_error(request, err,true);
		rc = NGX_ERROR;

	} else {
		for (concurrent = 0 ;concurrent < len ; concurrent++) {
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
               "Writing output of index %d to body",concurrent);
			if ( concurrent > 0 )
				append_to_output_chain(request,",");

			append_to_output_chain(request,request->array_res[concurrent]);
			free(request->array_res[concurrent]);
		}
	}

	ngx_pfree(request->r->pool,request->array_res);

	sem_destroy(sem);
	ngx_pfree(request->r->pool,sem);
			
	append_to_output_chain(request,"]");

	ubus_close_fds(request->ubus_ctx);

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
               "Request processed correctly");

	return rc;
}

static ngx_int_t ngx_http_ubus_elaborate_req(request_ctx_t *request, json_object *obj)
{
	ubus_ctx_t *ctx;
	enum rpc_status rc;

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
               "Analyzing json object");

	switch (obj ? json_object_get_type(obj) : json_type_null) {
		case json_type_object:

			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "Json object detected");

			ctx = ngx_pcalloc(request->r->pool,sizeof(ubus_ctx_t));
			setup_ubus_ctx_t(ctx,request,obj);

			rc = ubus_process_object(ctx);

			if ( rc != REQUEST_OK ) {
				ubus_single_error(request,rc,false);
				return NGX_ERROR;
			}

			ubus_close_fds(request->ubus_ctx);

			return NGX_OK;
		case json_type_array:

			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, request->r->connection->log, 0,
               "Json array detected");

			append_to_output_chain(request,"[");
			return ubus_process_array(request, obj);
		default:
			ubus_single_error(request, ERROR_PARSE,false);
			return NGX_ERROR;
	}
}

static void ngx_http_ubus_req_handler(ngx_http_request_t *r)
{
	ngx_int_t rc;
	off_t pos = 0;
	off_t len;
	ngx_chain_t  *in;
	struct dispatch_ubus *ubus;
	ngx_http_ubus_loc_conf_t  *cglcf;

	request_ctx_t *request;

	cglcf = ngx_http_get_module_loc_conf(r, ngx_http_ubus_module);
	char *buffer = ngx_pcalloc(r->pool, r->headers_in.content_length_n + 1);
	
	request = ngx_pcalloc(r->pool,sizeof(request_ctx_t));
	request->r = r;
	ubus = ngx_pcalloc(r->pool,sizeof(struct dispatch_ubus));
	ubus->jsobj = NULL;
	ubus->jstok = json_tokener_new();

	request->ubus_ctx = ubus_connect(cglcf->socket_path.data);

	if (ubus->jsobj || !ubus->jstok) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Error ubus struct not ok");
		ubus_single_error(request, ERROR_PARSE,false);
		ngx_http_finalize_request(r, NGX_HTTP_OK);
		return;
	}

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "Reading request body");

	for (in = r->request_body->bufs; in; in = in->next) {

		len = ngx_buf_size(in->buf);
		ngx_memcpy(buffer + pos,in->buf->pos,len);
		pos += len;

		if (pos > UBUS_MAX_POST_SIZE) {
			ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Error max post size for ubus socket");
			ubus_single_error(request, ERROR_PARSE,false);
			ngx_pfree(r->pool,buffer);
			ngx_http_finalize_request(r, NGX_HTTP_OK);
			return;
		}
	}

	if ( pos != r->headers_in.content_length_n ) {
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Readed buffer differ from header request len");
		ubus_single_error(request, ERROR_PARSE,false);
		ngx_pfree(r->pool,buffer);
		ngx_http_finalize_request(r, NGX_HTTP_OK);
		return;
	}

	ubus->jsobj = json_tokener_parse_ex(ubus->jstok, buffer, pos);
	ngx_pfree(r->pool,buffer);

	rc = ngx_http_ubus_elaborate_req(request,ubus->jsobj);

	free(ubus->jsobj);
	free(ubus->jstok);
	ngx_pfree(r->pool,ubus);

	if (rc == NGX_ERROR) {
		// With ngx_error we are sending json error 
		// and we say that the request is ok
		ngx_http_finalize_request(r, NGX_HTTP_OK);
		return;
	}

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "Sending header");

	rc = ngx_http_ubus_send_header(r,cglcf,NGX_HTTP_OK,request->res_len);
	if (rc == NGX_ERROR || rc > NGX_OK) {
		ngx_http_finalize_request(r, rc);
		return;
	}

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "Sending body");

	rc = ngx_http_ubus_send_body(request);

	ngx_pfree(r->pool,request);

	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
               "Request complete");

	ngx_http_finalize_request(r, rc);
}

static ngx_int_t
ngx_http_ubus_handler(ngx_http_request_t *r)
{
	ngx_int_t     rc;

	ngx_http_ubus_loc_conf_t  *cglcf;
	cglcf = ngx_http_get_module_loc_conf(r, ngx_http_ubus_module);
	
	switch (r->method)
	{
		case NGX_HTTP_OPTIONS:
			r->header_only = 1;
			ngx_http_ubus_send_header(r,cglcf,NGX_HTTP_OK,0);
			ngx_http_finalize_request(r,NGX_HTTP_OK);
			return NGX_DONE;

		case NGX_HTTP_POST:

			rc = ngx_http_read_client_request_body(r, ngx_http_ubus_req_handler);
			if (rc >= NGX_HTTP_SPECIAL_RESPONSE)
				return rc;

			return NGX_DONE;

		default:
			return NGX_HTTP_BAD_REQUEST;
	}
}

static char *
ngx_http_ubus(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
		ngx_http_core_loc_conf_t  *clcf;
		ngx_http_ubus_loc_conf_t *cglcf = conf;

		clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
		clcf->handler = ngx_http_ubus_handler;

		cglcf->enable = 1;

		return NGX_CONF_OK;
}

static void *
ngx_http_ubus_create_loc_conf(ngx_conf_t *cf)
{
		ngx_http_ubus_loc_conf_t  *conf;

		conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_ubus_loc_conf_t));
		if (conf == NULL) {
				return NGX_CONF_ERROR;
		}

		conf->socket_path.data = NULL;
		conf->socket_path.len = -1;

		conf->cors = NGX_CONF_UNSET;
		conf->noauth = NGX_CONF_UNSET;
		conf->script_timeout = NGX_CONF_UNSET_UINT;
		conf->parallel_req = NGX_CONF_UNSET_UINT;
		conf->enable = NGX_CONF_UNSET;
		return conf;
}

static char *
ngx_http_ubus_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
		ngx_http_ubus_loc_conf_t *prev = parent;
		ngx_http_ubus_loc_conf_t *conf = child;

		struct ubus_context *test_ubus;

		// Skip merge of other, if we don't have a socket to connect...
		// We don't init the module at all.
		if (conf->socket_path.data == NULL)
				return NGX_CONF_OK;

		ngx_conf_merge_value(conf->cors, prev->cors, 0);
		ngx_conf_merge_value(conf->noauth, prev->noauth, 0);
		ngx_conf_merge_uint_value(conf->script_timeout, prev->script_timeout, 60);
		ngx_conf_merge_value(conf->enable, prev->enable, 0);
		ngx_conf_merge_uint_value(conf->parallel_req, prev->parallel_req, 1);

		if (conf->script_timeout == 0 ) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "ubus_script_timeout must be greater than 0"); 
				return NGX_CONF_ERROR;
		}

		if (conf->parallel_req == 0 ) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "ubus_parallel_req must be greater than 0"); 
				return NGX_CONF_ERROR;
		}

		if (conf->enable) {
			test_ubus = ubus_connect(conf->socket_path.data);
			if (!test_ubus) {
				ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Unable to connect to ubus socket: %s", conf->socket_path.data);
				return NGX_CONF_ERROR;
			}
			ubus_close_fds(test_ubus);
		}

		return NGX_CONF_OK;
}