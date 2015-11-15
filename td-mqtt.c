#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <telldus-core.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <mosquitto.h>
#include <ctype.h>
#include <string.h>

/* rules to map raw events to publish invokations */

struct relay_rule {
        struct {
                char *key;
                char *value;
        } filters[5];
        struct {
                char *topicformat;
                char *messageformat;
                int qos;
                bool retain;
        } mqtt_template[5];
};

/* application context for callbacks */

struct context {
        struct mosquitto *mosq;
        int failures;
        int debug;
        const char *sub_prefix;
        const char *pub_prefix;
        int num_relay_rules;
        struct relay_rule *relay_rules;
};


/* mosquitto callbacks */

static void my_message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
        struct context *ctx = obj;
        if(ctx->debug > 1) {
                fprintf(stderr, "topic %s: payload %s\n", msg->topic, (const char*)msg->payload);
        }
        int intNumberOfDevices = tdGetNumberOfDevices();
        for (int i = 0; i < intNumberOfDevices; i++) {
                int id = tdGetDeviceId( i );
                char *name = tdGetName( id );
                char topic[100];
                snprintf(topic,sizeof(topic)-1,"%s/%s/method",ctx->sub_prefix,name);
                if(ctx->debug > 3) {
                        fprintf(stderr, "considering topic %s\n", topic);
                }
                if(!strcmp(topic,msg->topic)) {
                        if(ctx->debug > 2) {
                                fprintf(stderr, "topic %s match\n", topic);
                        }
                        if(!strncmp("turnon",(char*)msg->payload, msg->payloadlen)) {
                                tdTurnOn(id);
                        }
                        if(!strncmp("turnoff",(char*)msg->payload, msg->payloadlen)) {
                                tdTurnOff(id);
                        }
                        if(!strncmp("bell",(char*)msg->payload, msg->payloadlen)) {
                                tdBell(id);
                        }
                }
                tdReleaseString(name);
        }
}

void my_log_callback(struct mosquitto *mosq, void *context, int level, const char *message)
{
        struct context *ctx = context;
        switch(level) {
        case MOSQ_LOG_INFO:
        case MOSQ_LOG_NOTICE:
                if(ctx->debug < 2) return;
                break;
        case MOSQ_LOG_WARNING:
        case MOSQ_LOG_ERR:
                if(ctx->debug < 1) return;
                break;
        case MOSQ_LOG_DEBUG:
        case MOSQ_LOG_SUBSCRIBE:
        case MOSQ_LOG_UNSUBSCRIBE:
        default:
                if(ctx->debug < 3) return;
                break;
        };
        fprintf(stderr, "%s\n", message);
}

/* telldus_data  abstraction for querying key/value pairs from a raw_event */
struct telldus_data {
        char *msg;
};

static void telldus_data_init(struct telldus_data *data, const char *td_data)
{
        data->msg=strdup(td_data);
}

static void telldus_data_release(struct telldus_data *data)
{
        free(data->msg);
}

/* TODO: consider parsing into a map during init */
static size_t telldus_data_get_value(char *str, size_t size, const char *key, const struct telldus_data *data)
{
        size_t outsize = 0;
        enum {
                IN_KEY_OK,
                IN_KEY_BAD,
                IN_VALUE_COLLECT,
                IN_VALUE_SKIP
        } state = IN_KEY_OK;
        const char *msg = data->msg;

        const char *keymatch = key;

        for (;;) {
                switch(*msg) {
                case '\0':
                        if(size)
                                *str='\0';
                        return outsize;
                        break;
                case ';':
                        switch(state) {
                        case IN_KEY_OK:
                        case IN_KEY_BAD:
                                state = IN_KEY_OK;
                                keymatch = key;
                                break;
                        case IN_VALUE_COLLECT:
                                /* success , value complete */
                                if(size)
                                        *str='\0';
                                return outsize;
                                break;
                        case IN_VALUE_SKIP:
                                state = IN_KEY_OK;
                                keymatch = key;
                                break;
                        }
                        break;
                case ':':
                        switch(state) {
                        case IN_KEY_OK:
                        case IN_VALUE_COLLECT:  // assumes values can contain colons
                                state = IN_VALUE_COLLECT;
                                break;
                        case IN_KEY_BAD:
                        case IN_VALUE_SKIP:  // assumes values can contain colons
                                state = IN_VALUE_SKIP;
                                break;
                        }
                        break;
                default:
                        switch(state) {
                        case IN_KEY_OK: // keep on comparing
                                if(*keymatch != *msg) // mismatch
                                        state = IN_KEY_BAD;
                                if(*keymatch != '\0') // more left of expected key
                                        keymatch++;
                                break;
                        case IN_KEY_BAD: // keep on ignoring a bad key
                                break;
                        case IN_VALUE_COLLECT: // keep collecting the value of expected key
                                if(size) {
                                        *str = *msg;
                                        str++;
                                        outsize++;
                                        size--;
                                }
                                break;
                        case IN_VALUE_SKIP: // keep on ignoring irrelevant value
                                break;
                        default:
                                // should not happen
                                break;
                        }
                        break;

                }
                msg++;
        }
}

/* functions to format strings for topic and payload based on telldus_data */

static size_t format_verbatim(char **str, size_t *size, const char **format, struct telldus_data *data)
{
        if(size) {
                **str=**format;
                (*str)++;
                (*format)++;
                return 1;
        } else {
                return 0;
        }
}

static size_t format_variable(char **str, size_t *size, const char **format, struct telldus_data *data)
{
        char key[256];
        char *k = key;
        (*format)++; // $
        if(**format=='{') {
                (*format)++;
        }
        while(**format != '\0' && isalpha(**format)) {
                if(k<key+sizeof(key)-1) {
                        *k=**format;
                        k++;
                }
                (*format)++;
        }
        *k='\0';
        k++;
        if(**format=='}') {
                (*format)++;
        }
        //  size_t bytes = snprintf(*str, *size, "*%s*", key);
        char value[256];
        telldus_data_get_value(value,sizeof(value)-1,key,data);
        size_t bytes = snprintf(*str, *size, "%s", value);
        *str += bytes;
        *size -= bytes;
        return bytes;
}

static size_t format_message(char *str, size_t size, const char *format, struct telldus_data *data)
{
        size_t outsize = 0;
        for(;;) {
                switch(*format) {
                case '$':
                        outsize += format_variable(&str, &size, &format, data);
                        break;
                case '\0':
                        if(size) *str='\0';
                        return outsize;
                default:
                        outsize += format_verbatim(&str, &size, &format, data);
                        break;
                }
        }
}

/* telldus callback when receiving a message over radio */

static void raw_event(const char *data, int controllerId, int callbackId, void *context)
{
        struct context *ctx = context;
        struct relay_rule *relay_rules = ctx->relay_rules;
        if(ctx->debug>1)
                printf("raw_event: %s\n", data);

        struct telldus_data d;
        telldus_data_init(&d,data);
        int f = 0;
        for(f=0; f<ctx->num_relay_rules; f++) {
                int i = 0;
                int accepted = 1;
                // ensure all filters match in this relay_rule specification
                while(relay_rules[f].filters[i].key && relay_rules[f].filters[i].value && accepted) {
                        char expected[256];
                        telldus_data_get_value(expected, sizeof(expected)-1,
                                               relay_rules[f].filters[i].key, &d);
                        accepted &= !strcmp(expected, relay_rules[f].filters[i].value);
                        i++;
                }
                int j = 0;
                if(accepted) {
                        // matched, now format and deliver messages on all associated mqtt_templates
                        while(relay_rules[f].mqtt_template[j].topicformat &&
                              relay_rules[f].mqtt_template[j].messageformat &&
                                ! ctx->failures) {
                                char topic[1024];
                                char publish_topic[1024];
                                char message[256];
                                format_message(topic, sizeof(topic)-1,
                                               relay_rules[f].mqtt_template[j].topicformat, &d);
                                format_message(message, sizeof(topic)-1,
                                               relay_rules[f].mqtt_template[j].messageformat, &d);
                                snprintf(publish_topic,sizeof(publish_topic),
                                         "%s%s",ctx->pub_prefix,topic);
                                int rc2;
                                static int mid_sent = 0;
                                rc2 = mosquitto_publish(ctx->mosq,
                                                        &mid_sent,
                                                        publish_topic,
                                                        strlen(message),
                                                        (uint8_t *)message,
                                                        relay_rules[f].mqtt_template[j].qos,
                                                        relay_rules[f].mqtt_template[j].retain);
                                if(rc2) {
                                        fprintf(stderr, "Error: mosquitto_publish() returned %d: %s.\n", rc2, mosquitto_strerror(rc2));
// ignore failures for now, telldus callbacks can appear before MQTT connection is established
//                                        ctx->failures++;
                                }
                                if(ctx->debug>3)
                                        fprintf(stderr, "Done with template %d.\n", j);
                                j++;
                        }
                }
        }
        telldus_data_release(&d);

}


/* default rules for mapping raw events into payloads and topics to publish to */

struct relay_rule default_relay_rules[] = {
        {
                { { "protocol", "mandolyn" } },
                {       { "${class}/${protocol}/${model}/${id}/temp","${temp}", 0, 0 },
                        { "${class}/${protocol}/${model}/${id}/humidity","${humidity}", 0, 0}
                }
        },
        {
                { { "protocol", "waveman" } },
                { { "${class}/${protocol}/${model}/${house}/${unit}/method","${method}", 0, 0 } }
        },
        {
                { { "protocol", "fineoffset" } },
                { { "${class}/${protocol}/${model}/${id}/${unit}/temp","${temp}", 0, 0 } }
        },
        {
                { { "protocol", "arctech" }, { "model", "selflearning" } },
                { { "${class}/${protocol}/${model}/${house}/${unit}/${group}/method","${method}", 0, 0 } }
        }

};

/* main */

int main(int argc, char *argv[])
{
        int port = 1883;
        int reconnect_delay = 1;
        char *host = "localhost";
        char subscription[128];
        int rawcallback;
        struct context *ctx = malloc(sizeof(struct context)+sizeof(default_relay_rules));
        ctx->debug = 0;
        ctx->failures = 0;
        ctx->relay_rules=default_relay_rules; // TODO read relay rules from a configuration file
        ctx->num_relay_rules=sizeof(default_relay_rules)/sizeof(*default_relay_rules);
        int opt;
        ctx->sub_prefix = "telldus";
        ctx->pub_prefix = "";
        struct relay_rule *relay_rules = ctx->relay_rules;

        snprintf(subscription,sizeof(subscription)-1,"%s/#",ctx->sub_prefix);

        while ((opt = getopt(argc, argv, "vS:d:h:p:P:")) != -1) {
                switch (opt) {
                case 'v':
                        ctx->debug++;
                        break;
                case 'h':
                        host=strdup(optarg);
                        break;
                case 'S':
                        ctx->sub_prefix=strdup(optarg);
                        snprintf(subscription,sizeof(subscription)-1,"%s/#",ctx->sub_prefix);
                        break;
                case 'P':
                        ctx->pub_prefix=strdup(optarg);
                        break;
                case 'p':
                        port=atoi(optarg);
                        break;
                case 'd':
                        reconnect_delay=atoi(optarg);
                        break;
                default: /* '?' */
                        fprintf(stderr, "Usage: %s [-v] "
                                "[-h <host>] "
                                "[-p <port>]\n\t"
                                "[-S <subscription topic prefix>] "
                                "[-P <publishing topic prefix>]\n\t"
                                "[-d <reconnect after n seconds> ]\n\n"
                                "\t%s connects to MQTT broker at %s:%d.\n\n"
                                "\tIt subscribes messages for topic '%s'.\n"
                                "\tWhen a 'turnon', 'turnoff' or 'bell' message is received at %s/<device>/method it will trigger\n"
                                "\tthe corresponding operation on a Telldus device with the same name.\n",
                                argv[0],
                                argv[0], host, port,
                                subscription,
                                ctx->sub_prefix);

                        fprintf(stderr,
                                "\n\tIt listens for raw events from Telldus.\n");
                        int f;
                        for(f=0; f<ctx->num_relay_rules; f++) {
                                fprintf(stderr, "\tWhen it receives a raw event where ");
                                int i = 0;
                                const char *separator="";
                                while(ctx->relay_rules[f].filters[i].key &&
                                      relay_rules[f].filters[i].value) {
                                        fprintf(stderr, "%sfield '%s' value is '%s'",
                                                separator,
                                                relay_rules[f].filters[i].key,
                                                relay_rules[f].filters[i].value);
                                        i++;
                                        separator = relay_rules[f].filters[i+1].key ? ", " : " and ";
                                }
                                separator="";
                                fprintf(stderr, "\n\t\tit publishes ");
                                int j = 0;
                                while(relay_rules[f].mqtt_template[j].topicformat &&
                                      relay_rules[f].mqtt_template[j].messageformat) {
                                        fprintf(stderr, "%sa message '%s' on topic '%s%s'",
                                                separator,
                                                relay_rules[f].mqtt_template[j].messageformat,
                                                ctx->pub_prefix,
                                                relay_rules[f].mqtt_template[j].topicformat);
                                        j++;
                                        separator = relay_rules[f].mqtt_template[j+1].topicformat ? ", \n\t\t" : " and \n\t\t";
                                }
                                fprintf(stderr, "\n");
                        }


                        exit(EXIT_FAILURE);
                }
        }

        tdInit();

        // TODO: move after mqtt connection has been established when unregistering and re-registering works ok
        rawcallback = tdRegisterRawDeviceEvent(raw_event,ctx);

        do {
                ctx->failures = 0; // TODO: synchronization for telldus threading

                char hostname[21];
                char id[30];

                memset(hostname, 0, sizeof(hostname));
                gethostname(hostname, sizeof(hostname)-1);
                snprintf(id, sizeof(id)-1, "mosq_pub_%d_%s", getpid(), hostname);

                mosquitto_lib_init();
                ctx->mosq = mosquitto_new(id, true, ctx);
                if(!ctx->mosq) {
                        fprintf(stderr, "Error: Out of memory.\n");
                        return 1;
                }

                if(ctx->debug > 0) {
                        mosquitto_log_callback_set(ctx->mosq,
                                                   my_log_callback);
                }

                int rc;
                rc = mosquitto_connect(ctx->mosq, host, port, 30);
                if(rc) {
                        if(ctx->debug > 0) {
                                fprintf(stderr, "failed to connect %s:%d\n", host, port);
                        }
                        goto clean;
                }
                mosquitto_message_callback_set(ctx->mosq, my_message_callback);

                rc = mosquitto_subscribe(ctx->mosq, NULL, subscription, 0);
                if(rc) {
                        if(ctx->debug > 0) {
                                fprintf(stderr, "failed to subscribe %s\n", subscription);
                        }
                        goto clean;
                }


                do {
                        rc = mosquitto_loop(ctx->mosq, 60, 1);
                } while(rc == MOSQ_ERR_SUCCESS && !ctx->failures);

clean:

                mosquitto_destroy(ctx->mosq);
                mosquitto_lib_cleanup();
        } while(reconnect_delay >= 0 && ( sleep(reconnect_delay), true));

        tdUnregisterCallback(rawcallback);
        tdClose();
        return 0;
}
