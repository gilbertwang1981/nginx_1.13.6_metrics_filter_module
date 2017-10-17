#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static ngx_http_output_body_filter_pt ngx_http_next_body_filter;
static ngx_http_output_header_filter_pt ngx_http_next_header_filter;

typedef struct {
    ngx_flag_t enable;
}ngx_http_metrics_filter_conf_t;

static ngx_hash_init_t hinit;

static ngx_command_t  ngx_http_metrics_filter_commands[] = {
    { 
    	ngx_string("ngx_http_metrics_filter_modules"),
      	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_FLAG,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_metrics_filter_conf_t , enable),
        NULL
    },
    ngx_null_command
};

static ngx_int_t ngx_http_metrics_filter_post_conf(ngx_conf_t * conf);
static void * ngx_http_metrics_filter_create_conf(ngx_conf_t *cf);
static char * ngx_http_metrics_filter_merge_conf(ngx_conf_t *cf,void*parent,void*child);
static ngx_int_t ngx_http_metrics_filter_body_filter(ngx_http_request_t *r, ngx_chain_t *in);
static ngx_int_t ngx_http_metrics_filter_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_metrics_filter_init_module(ngx_cycle_t * cycle);
static ngx_int_t ngx_initialize_metrics(ngx_array_t * metrics  , ngx_log_t * log , ngx_pool_t * pool);

static ngx_http_module_t  ngx_http_metrics_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_metrics_filter_post_conf,     /* postconfiguration */
    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */
    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */
    ngx_http_metrics_filter_create_conf,   /* create location configuration */
    ngx_http_metrics_filter_merge_conf     /* merge location configuration */
};

ngx_module_t ngx_http_metrics_filter_modules = {
    NGX_MODULE_V1,
    &ngx_http_metrics_filter_module_ctx,         /* module context */
    ngx_http_metrics_filter_commands,            /* module directives */
    NGX_HTTP_MODULE,                       		 /* module type */
    NULL,         						   		 /* init master */
    ngx_http_metrics_filter_init_module,         /* init module */
    NULL,             					   		 /* init process */
    NULL,                                  		 /* init thread */
    NULL,                                  		 /* exit thread */
    NULL,             					   		 /* exit process */
    NULL,                                  		 /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t ngx_initialize_metrics(ngx_array_t * metrics , ngx_log_t * log , ngx_pool_t * pool) {
	hinit.max_size = 1024;
	hinit.bucket_size = 512;
	hinit.name = "metrics hash table";
	hinit.pool = pool;
	hinit.key = &ngx_hash_key;
	hinit.hash = NULL;
	hinit.temp_pool = NULL;

	ngx_hash_key_t * pos = (ngx_hash_key_t *)((u_char *)metrics->elts);
	if (ngx_hash_init(&hinit , pos , (int)metrics->nelts) != NGX_OK || 
		hinit.hash == NULL) {
		ngx_log_error(NGX_LOG_ERR , log , 0 , "create hash table failed.");

		return NGX_ABORT;
	}

	u_char * key = ngx_palloc(pool , sizeof(u_char) * 32);
	ngx_sprintf(key , "key_%d" , 2);
	ngx_str_t str; 
	ngx_str_set(&str , key);

	u_char * value = (u_char *)ngx_hash_find(
		hinit.hash , ngx_hash_key(key , ngx_strlen(key)) , 
		str.data, str.len);
	if (value != NULL) {
		ngx_log_error(NGX_LOG_INFO , log , 0 , "Got the data from hash table, %s" , value);
	}

	return NGX_OK;
}

static ngx_int_t ngx_http_metrics_filter_init_module(ngx_cycle_t * cycle) {
	ngx_log_error(NGX_LOG_INFO , cycle->log , 0 , "<initialize module>");

	ngx_array_t * array = ngx_array_create(cycle->pool , 3 , sizeof(ngx_hash_key_t));
	
	ngx_hash_key_t * pos = (ngx_hash_key_t *)((u_char *)array->elts);
	int i = 0 ;
	for (i = 0; i < 3 ; i ++) {		
		u_char * key = ngx_palloc(cycle->pool , sizeof(u_char) * 32);
		ngx_sprintf(key , "key_%d" , i);
		ngx_str_set(&(pos->key) , key);
		pos->value = key;

		pos->key_hash = ngx_hash_key(key , ngx_strlen(key));
		
		array->nelts ++;

		pos ++;
	}	

	return ngx_initialize_metrics(array , cycle->log , cycle->pool);
}

static void* ngx_http_metrics_filter_create_conf(ngx_conf_t *cf){	
    ngx_http_metrics_filter_conf_t  * mycf = (ngx_http_metrics_filter_conf_t  *)
		ngx_pcalloc(cf->pool, sizeof(ngx_http_metrics_filter_conf_t));
    if (mycf == NULL) {
        return NULL;
    }

    mycf->enable = NGX_CONF_UNSET;
	
    return mycf;
}

static char * ngx_http_metrics_filter_merge_conf(ngx_conf_t *cf,void*parent,void*child) {
    ngx_http_metrics_filter_conf_t *prev = (ngx_http_metrics_filter_conf_t *)parent;
    ngx_http_metrics_filter_conf_t *conf = (ngx_http_metrics_filter_conf_t *)child;

    ngx_conf_merge_value(conf->enable, prev->enable, 1);

    return NGX_CONF_OK;
}

static ngx_int_t ngx_http_metrics_filter_header_filter(ngx_http_request_t *r) {
    if (r->headers_out.status != NGX_HTTP_OK) {
		ngx_log_error(NGX_LOG_INFO , r->connection->log, 0, "<failed:%d>" , r->headers_out.status);
    } else {
		ngx_log_error(NGX_LOG_INFO , r->connection->log, 0, "<success>");
	}

    return ngx_http_next_header_filter(r);
}


static ngx_int_t ngx_http_metrics_filter_body_filter(ngx_http_request_t *r, ngx_chain_t *in) {
    return ngx_http_next_body_filter(r, in);
}

static ngx_int_t ngx_http_metrics_filter_post_conf(ngx_conf_t * conf) {
	ngx_http_next_header_filter=ngx_http_top_header_filter;
	ngx_http_top_header_filter=ngx_http_metrics_filter_header_filter;	

	ngx_http_next_body_filter = ngx_http_top_body_filter;
	ngx_http_top_body_filter = ngx_http_metrics_filter_body_filter;

    return NGX_OK;
}



