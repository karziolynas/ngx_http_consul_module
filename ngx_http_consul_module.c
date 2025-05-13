#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

typedef struct {
    // the array with service ids and variables
    ngx_array_t *commands;
    // the handler for the return data function
    u_char* (*fetchAddress)(u_char *);
} ngx_http_consul_main_conf_t;

typedef struct {
    // the name of the variable to be used in the configuration
    ngx_str_t addr_var;
    // the service id passed in the configuration file 
    ngx_str_t service_id;       
} ngx_consul_command_t;


//declaring methods
static char *ngx_http_consul_init_variables(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_address_handler(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) ;
static ngx_int_t ngx_http_consul_init_process(ngx_cycle_t *cycle);
static void *ngx_http_consul_create_main_conf(ngx_conf_t *cf);

// module context
static ngx_http_module_t ngx_http_consul_ctx = {
    NULL,
    NULL,
    ngx_http_consul_create_main_conf, 
    NULL, 
    NULL, //ngx_http_consul_create_srv_conf, 
    NULL, 
    NULL, //ngx_http_consul_create_loc_conf, 
    NULL
};

// Module directives
static ngx_command_t ngx_http_consul_commands[] = {
    { 
        ngx_string("consulServiceInit"),
        NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE2,
        ngx_http_consul_init_variables,
        NGX_HTTP_MAIN_CONF_OFFSET,
        0,
        NULL 
    },
    ngx_null_command
};

// Module definition
ngx_module_t ngx_http_consul_module = {
    NGX_MODULE_V1,
    &ngx_http_consul_ctx, 
    ngx_http_consul_commands,
    NGX_HTTP_MODULE, 
    NULL, 
    NULL, 
    //starts a procces which loads in the shared library
    ngx_http_consul_init_process,  /* init process */
    NULL, 
    NULL, 
    NULL, 
    NULL,
    NGX_MODULE_V1_PADDING
};

// creates a main configuration and allocates memory for the struct and array
static void *ngx_http_consul_create_main_conf(ngx_conf_t *cf) {
    ngx_http_consul_main_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_consul_main_conf_t));
    if (conf == NULL) {
        return NULL;
    }
    conf->commands = ngx_array_create(cf->pool, 4, sizeof(ngx_consul_command_t));
    if (conf->commands == NULL) {
        return NULL;
    }
    return conf;
}

// takes the variables passed in the nginx.conf file and saves them
static char *ngx_http_consul_init_variables(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_str_t *args = cf->args->elts;

    // checks if the commands is used correctly
    if (cf->args->nelts != 3) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Usage: consulServicesInit varName '<ServiceId>'");
        return NGX_CONF_ERROR;
    }
    ngx_http_consul_main_conf_t *cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_consul_module);

    // populates one entry of the array
    ngx_consul_command_t *entry = ngx_array_push(cmcf->commands);
    entry->service_id = args[1];
    entry->addr_var = args[2];
    
    // adds the variable
    ngx_http_variable_t *address;
    address = ngx_http_add_variable(cf, &entry->addr_var, NGX_HTTP_VAR_CHANGEABLE);
    if (address == NULL) {
        return NGX_CONF_ERROR;
    }

    // assigns the handler function 
    // and the service id to be called with the return data function
    address->data = (uintptr_t)&entry->service_id;
    address->get_handler = ngx_http_address_handler;

    ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0, "Added an address variable");
    
    return NGX_CONF_OK;

}

// starts a worker procces after the master proccess has initialized/configured the module
static ngx_int_t ngx_http_consul_init_process(ngx_cycle_t *cycle) {
    void* handle;
    int (*dataLoop)(u_char*);
    int result;
    ngx_http_consul_main_conf_t *cmcf;
    
    // gets the service ID from configuration
    cmcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_consul_module);

    // loads shared library
    handle = dlopen("/usr/local/nginx/goCode/goConcurrentServices.so", RTLD_LAZY);
    if (!handle) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "Failed to load shared library: %s", dlerror());
        return NGX_ERROR;
    }
    // gets the method that starts the goroutine
    dataLoop = dlsym(handle, "GetConsulAddresses");
    if (!dataLoop) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "dlsym(GetConsulAddresses) failed: %s", dlerror());
        dlclose(handle);
        return NGX_ERROR;
    }

    // gets the method that returns the in memory updated variable 
    cmcf->fetchAddress = dlsym(handle, "ReturnAddress");
    if (!cmcf->fetchAddress) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "dlsym(ReturnAddress) failed: %s", dlerror());
        dlclose(handle);
        return NGX_ERROR;
    }
    
    ngx_consul_command_t *entries = cmcf->commands->elts;

    // starts a go routine based of each time the command is used
    // e.g. 2 commands with different service ids
    // consulServiceInit serviceID.tag address
    // serviceID.tag - fetches all healthy services based of the id and tags
    // address - saves the healthy service address to this variable (later used with proxy_pass)
    for (ngx_uint_t i = 0; i < cmcf->commands->nelts; i++) {
        result = dataLoop(entries[i].service_id.data);
        if (result != 1) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "Failed to initialize socket server");
            dlclose(handle);
            return NGX_ERROR;
        }
    }   
    ngx_msleep(200);
    
    return NGX_OK;
}

// the handler - called on every request
static ngx_int_t ngx_http_address_handler(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_str_t *serviceId = (ngx_str_t *) data;

    ngx_http_consul_main_conf_t *cmcf = ngx_http_get_module_main_conf(r, ngx_http_consul_module);

    if (!cmcf || !cmcf->fetchAddress) {
        return NGX_ERROR;
    }

    // fetches address based on the service_id
    u_char* address = cmcf->fetchAddress(serviceId->data);

    if(address == NULL || ngx_strlen(address) == 0){
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "No address from go.");
        v->not_found = 1;
        return NGX_ERROR;
    }

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0, "Consul returned address: %s", address);

    ngx_str_t ngx_address = { ngx_strlen(address), address };

    // allocates memory for the 'address' variable and copies the address into it
    v->data = ngx_palloc(r->pool, ngx_address.len);
    if (v->data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(v->data, ngx_address.data, ngx_address.len);
    v->len = ngx_address.len;
    v->valid = 1; 
    v->not_found = 0;
    v->no_cacheable = 1;
    return NGX_OK;
}
