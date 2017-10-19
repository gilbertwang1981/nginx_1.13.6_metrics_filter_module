#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <pthread.h>
#include <sys/mman.h>

#define MAX_URL_CONFIG_SIZE 512

typedef struct tag_ngx_http_metrics_filter_conf {
    ngx_flag_t enable;
}ngx_http_metrics_filter_conf_t;

typedef struct tag_ngx_http_status_code_map {
	int code;
	int index;
	struct tag_ngx_http_status_code_map * next;
} ngx_http_status_code_map_t;

typedef struct tag_ngx_http_metrics_map {
	u_char * url;
	ngx_http_status_code_map_t * status_code;
	struct tag_ngx_http_metrics_map * next;
} ngx_http_metrics_map_t;

static ngx_http_output_body_filter_pt ngx_http_next_body_filter;
static ngx_http_output_header_filter_pt ngx_http_next_header_filter;

static int MAX_METRICS_NUM = 4;
static int fd = -1;
static char * mem_ptr = 0;
static int is_fork = 0;
static ngx_http_metrics_map_t * status_code_map = NULL;

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

static ngx_int_t ngx_http_init_metrics_map();
static int ngx_http_get_metrics_index_by_url_code(u_char * url , int code , ngx_log_t * log);
static ngx_int_t ngx_http_metrics_filter_post_conf(ngx_conf_t * conf);
static void * ngx_http_metrics_filter_create_conf(ngx_conf_t *cf);
static char * ngx_http_metrics_filter_merge_conf(ngx_conf_t *cf,void*parent,void*child);
static ngx_int_t ngx_http_metrics_filter_body_filter(ngx_http_request_t *r, ngx_chain_t *in);
static ngx_int_t ngx_http_metrics_filter_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_metrics_filter_init_module(ngx_cycle_t * cycle);
static ngx_int_t ngx_initialize_metrics(ngx_log_t * log);

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

static ngx_int_t ngx_http_init_metrics_map() {
	ngx_http_status_code_map_t * status_code0 = 
			(ngx_http_status_code_map_t *)malloc(sizeof(ngx_http_status_code_map_t));

	status_code0->code = 200;
	status_code0->index = 0;

	ngx_http_status_code_map_t * status_code1 = 
			(ngx_http_status_code_map_t *)malloc(sizeof(ngx_http_status_code_map_t));

	status_code1->code = 404;
	status_code1->index = 1;
	status_code1->next = 0;

	status_code0->next = status_code1;

	ngx_http_metrics_map_t * sc_map = (ngx_http_metrics_map_t *)malloc(sizeof(ngx_http_metrics_map_t));
	sc_map->status_code = status_code0;

	sc_map->url = malloc(MAX_URL_CONFIG_SIZE);
	ngx_memset(sc_map->url , 0x00 , MAX_URL_CONFIG_SIZE);
	ngx_memcpy(sc_map->url , "test" , ngx_strlen("test"));
	sc_map->next = NULL;

	status_code_map = sc_map;
	
	return NGX_OK;
}

static int ngx_http_get_metrics_index_by_url_code(u_char * url , int code , ngx_log_t * log) {
	ngx_http_metrics_map_t * header = status_code_map;
	while (header != NULL) {
		ngx_log_error(NGX_LOG_INFO , log , 0 , "[uri:%s] [status:%d] [pattern:%s]" , 
		url , code , header->url);
		if (ngx_strstr(url , header->url) != 0) {
			ngx_http_status_code_map_t * sc_map = header->status_code;
			while (sc_map != NULL) {
				if (sc_map->code == code) {
					return sc_map->index;
				}
						
				sc_map = sc_map->next;
			}
		}
		
		header = header->next;
	}
	
	return -1;
}

static void * collector(void * args) {
	ngx_log_t * log = (ngx_log_t *)args;

	int is_e = 0;
	struct stat buf;
	if (stat("metrics.dat" , &buf) == -1) {
		is_e = 1;
	}

	if (fd == -1) {
		fd = open("metrics.dat" , O_RDWR | O_CREAT);
		if (fd == -1) {
			ngx_log_error(NGX_LOG_ERR , log , 0 , "open file failed.%s" , strerror(errno));
				
			return NULL;
		}

		if (is_e == 1) {
			int init = 0;
			write(fd , &init , MAX_METRICS_NUM * 4);
		}
	}	

	mem_ptr = (char *)mmap(0 , MAX_METRICS_NUM * 4 , PROT_READ | PROT_WRITE , MAP_SHARED , fd , 0);
	if (mem_ptr == MAP_FAILED) {
		close(fd);
		fd = -1;
	}
	
	if (is_e == 1) {
		(void)memset(mem_ptr , 0x00 , MAX_METRICS_NUM * 4);
	}

	while (1) {	
		sleep(1);

		int i = 0;
		for (;i < MAX_METRICS_NUM;i ++) {
			ngx_log_error(NGX_LOG_INFO , log , 0 , "counter:%d_%d" , *((int *)(mem_ptr + i * 4)) , i);
		}

		(void)memset(mem_ptr , 0x00 , MAX_METRICS_NUM * 4);
	}

	close(fd);
	fd = -1;
	munmap(mem_ptr , MAX_METRICS_NUM * 4);
	
	return 0;
}

static ngx_int_t ngx_initialize_metrics(ngx_log_t * log) {
	pthread_t tid;
	if (pthread_create(&tid , 0 , collector , log) == -1) {
		ngx_log_error(NGX_LOG_ERR , log , 0 , "create collector thread failed." );
		
		return NGX_ABORT;
	}

	return NGX_OK;
}

static ngx_int_t ngx_http_metrics_filter_init_module(ngx_cycle_t * cycle) {
	ngx_log_error(NGX_LOG_INFO , cycle->log , 0 , "<initialize metrics filter module>");

	return ngx_initialize_metrics(cycle->log);
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
	if (is_fork == 0) {
		if (ngx_http_init_metrics_map() != NGX_OK) {
			return NGX_ABORT;
		}
		
		if (fd == -1) {
			fd = open("metrics.dat" , O_RDWR);
			if (fd == -1) {
				return NGX_ABORT;
			}
		}

		mem_ptr = (char *)mmap(0 , MAX_METRICS_NUM * 4 , PROT_READ | PROT_WRITE , MAP_SHARED , fd , 0);
		if (mem_ptr == MAP_FAILED) {
			close(fd);
			fd = -1;

			return NGX_ABORT;
		}

		is_fork = 1;
	}

	int index = ngx_http_get_metrics_index_by_url_code(r->uri.data , r->headers_out.status , r->connection->log);
	ngx_log_error(NGX_LOG_INFO , r->connection->log , 0 , "index is %d." , index);
	
	if (index > MAX_METRICS_NUM || index == -1) {
		return ngx_http_next_header_filter(r);
	} else {
		int * pos = (int *)(mem_ptr + index * 4);
		*pos += 1;
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



