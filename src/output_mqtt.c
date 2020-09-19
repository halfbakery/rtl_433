/** @file
    MQTT output for rtl_433 events

    Copyright (C) 2019 Christian Zuckschwerdt

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

// note: our unit header includes unistd.h for gethostname() via data.h
#include "output_mqtt.h"
#include "optparse.h"
#include "util.h"
#include "fatal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mongoose.h"

/* MQTT client abstraction */

typedef void (*mqtt_publish_cb)(const struct mg_str *topic, const struct mg_str *payload);

typedef struct mqtt_client {
    struct mg_send_mqtt_handshake_opts opts;
    int prev_status;
    char address[253 + 6 + 1]; // dns max + port
    char client_id[256];
    uint16_t message_id;
    int publish_flags; // MG_MQTT_RETAIN | MG_MQTT_QOS(0)
    size_t num_subscriptions;
    struct mg_mqtt_topic_expression *subscriptions;
    mqtt_publish_cb *publish_callbacks;
} mqtt_client_t;

static void mqtt_client_event(struct mg_connection *nc, int ev, void *ev_data)
{
    // note that while shutting down the ctx is NULL
    mqtt_client_t *ctx = (mqtt_client_t *)nc->mgr->user_data;
    // only valid in MG_EV_MQTT_ events
    struct mg_mqtt_message *msg = (struct mg_mqtt_message *)ev_data;

    //if (ev != MG_EV_POLL)
    //    fprintf(stderr, "MQTT user handler got event %d\n", ev);

    switch (ev) {
    case MG_EV_CONNECT: {
        int connect_status = *(int *)ev_data;
        if (connect_status == 0) {
            // Success
            fprintf(stderr, "MQTT Connected...\n");
            mg_set_protocol_mqtt(nc);
            if (ctx)
                mg_send_mqtt_handshake_opt(nc, ctx->client_id, ctx->opts);
        }
        else {
            // Error, print only once
            if (ctx && ctx->prev_status != connect_status)
                fprintf(stderr, "MQTT connect error: %s\n", strerror(connect_status));
        }
        if (ctx)
            ctx->prev_status = connect_status;
        break;
    }
    case MG_EV_MQTT_CONNACK:
        if (msg->connack_ret_code != MG_EV_MQTT_CONNACK_ACCEPTED) {
            fprintf(stderr, "MQTT Connection error: %d\n", msg->connack_ret_code);
        }
        else {
            fprintf(stderr, "MQTT Connection established.\n");
	    mg_mqtt_subscribe(nc, ctx->subscriptions, ctx->num_subscriptions, ++ctx->message_id);
        }
        break;
    case MG_EV_MQTT_PUBACK:
        fprintf(stderr, "MQTT Message publishing acknowledged (msg_id: %d)\n", msg->message_id);
        break;
    case MG_EV_MQTT_SUBACK:
        fprintf(stderr, "MQTT Subscription acknowledged.\n");
        break;
    case MG_EV_MQTT_PUBLISH: {
        fprintf(stderr, "MQTT Incoming message %.*s: %.*s\n", (int)msg->topic.len,
                msg->topic.p, (int)msg->payload.len, msg->payload.p);
	for (size_t i = 0; i < ctx->num_subscriptions; i++) {
	    if (mg_mqtt_vmatch_topic_expression(ctx->subscriptions[i].topic, msg->topic)) {
		if (ctx->publish_callbacks[i])
		    ctx->publish_callbacks[i](&msg->topic, &msg->payload);
	    }
	}
        break;
    }
    case MG_EV_CLOSE:
        if (!ctx)
            break; // shuttig down
        if (ctx->prev_status == 0)
            fprintf(stderr, "MQTT Connection failed...\n");
        // reconnect
        if (mg_connect(nc->mgr, ctx->address, mqtt_client_event) == NULL) {
            fprintf(stderr, "MQTT connect(%s) failed\n", ctx->address);
        }
        break;
    }
}

static struct mg_mgr *mqtt_client_init(char const *host, char const *port, char const *user, char const *pass, char const *client_id, int retain)
{
    struct mg_mgr *mgr = calloc(1, sizeof(*mgr));
    if (!mgr)
        FATAL_CALLOC("mqtt_client_init()");

    mqtt_client_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        FATAL_CALLOC("mqtt_client_init()");

    ctx->opts.user_name = user;
    ctx->opts.password  = pass;
    ctx->publish_flags  = MG_MQTT_QOS(0) | (retain ? MG_MQTT_RETAIN : 0);
    // TODO: these should be user configurable options
    //ctx->opts.keepalive = 60;
    //ctx->timeout = 10000L;
    //ctx->cleansession = 1;
    strncpy(ctx->client_id, client_id, sizeof(ctx->client_id));

    mg_mgr_init(mgr, ctx);

    // if the host is an IPv6 address it needs quoting
    if (strchr(host, ':'))
        snprintf(ctx->address, sizeof(ctx->address), "[%s]:%s", host, port);
    else
        snprintf(ctx->address, sizeof(ctx->address), "%s:%s", host, port);

    if (mg_connect(mgr, ctx->address, mqtt_client_event) == NULL) {
        fprintf(stderr, "MQTT connect(%s) failed\n", ctx->address);
        exit(1);
    }

    return mgr;
}

static int mqtt_client_poll(struct mg_mgr *mgr)
{
    return mg_mgr_poll(mgr, 0);
}

static void mqtt_client_publish(struct mg_mgr *mgr, char const *topic, char const *str)
{
    mqtt_client_t *ctx = (mqtt_client_t *)mgr->user_data;
    ctx->message_id++;

    for (struct mg_connection *c = mg_next(mgr, NULL); c != NULL; c = mg_next(mgr, c)) {
        if (c->proto_handler)
            mg_mqtt_publish(c, topic, ctx->message_id, ctx->publish_flags, str, strlen(str));
    }
}

static int mqtt_client_subscribe(struct mg_mgr *mgr, const char *topic, uint8_t qos, mqtt_publish_cb cb)
{
    mqtt_client_t *ctx = (mqtt_client_t *)mgr->user_data;

    size_t i = ctx->num_subscriptions++;
    ctx->subscriptions = realloc(ctx->subscriptions, ctx->num_subscriptions * sizeof(struct mg_mqtt_topic_expression));
    ctx->publish_callbacks = realloc(ctx->publish_callbacks, ctx->num_subscriptions * sizeof(mqtt_publish_cb));
    if (!ctx->subscriptions || !ctx->publish_callbacks) {
	WARN_MALLOC("mqtt_client_subscribe()");
	return -1;
    }
    ctx->subscriptions[i].topic = strdup(topic);
    ctx->subscriptions[i].qos = qos;
    ctx->publish_callbacks[i] = cb;

    return 0;
}

static void mqtt_client_free(struct mg_mgr *mgr)
{
    mqtt_client_t *ctx = (mqtt_client_t *)mgr->user_data;

    for (size_t i = 0; i < ctx->num_subscriptions; i++) {
	free((char *) ctx->subscriptions[i].topic);
    }
    free(ctx->subscriptions);
    free(ctx->publish_callbacks);
    ctx->subscriptions = NULL;
    ctx->publish_callbacks = NULL;

    free(mgr->user_data);
    mgr->user_data = NULL;
    mg_mgr_free(mgr);
    free(mgr);
}

/* Helper */

/// clean the topic inplace to [-.A-Za-z0-9], esp. not whitespace, +, #, /, $
static char *mqtt_sanitize_topic(char *topic)
{
    for (char *p = topic; *p; ++p)
        if (*p != '-' && *p != '.' && (*p < 'A' || *p > 'Z') && (*p < 'a' || *p > 'z') && (*p < '0' || *p > '9'))
            *p = '_';

    return topic;
}

/* MQTT printer */

typedef struct {
    struct data_output output;
    struct mg_mgr *mgr;
    char topic[256];
    char hostname[64];
    char *devices;
    char *events;
    char *states;
    //char *homie;
    //char *hass;
} data_output_mqtt_t;

data_output_mqtt_t	*active_mqtt = NULL;

static void print_mqtt_array(data_output_t *output, data_array_t *array, char const *format)
{
    data_output_mqtt_t *mqtt = (data_output_mqtt_t *)output;

    char *orig = mqtt->topic + strlen(mqtt->topic); // save current topic

    for (int c = 0; c < array->num_values; ++c) {
        sprintf(orig, "/%d", c);
        print_array_value(output, array, format, c);
    }
    *orig = '\0'; // restore topic
}

static char *append_topic(char *topic, data_t *data)
{
    if (data->type == DATA_STRING) {
        strcpy(topic, data->value.v_ptr);
        mqtt_sanitize_topic(topic);
        topic += strlen(data->value.v_ptr);
    }
    else if (data->type == DATA_INT) {
        topic += sprintf(topic, "%d", data->value.v_int);
    }
    else {
        fprintf(stderr, "Can't append data type %d to topic\n", data->type);
    }

    return topic;
}

static char *expand_topic(char *topic, char const *format, data_t *data, char const *hostname)
{
    // collect well-known top level keys
    data_t *data_type    = NULL;
    data_t *data_model   = NULL;
    data_t *data_subtype = NULL;
    data_t *data_channel = NULL;
    data_t *data_id      = NULL;
    data_t *data_protocol = NULL;
    for (data_t *d = data; d; d = d->next) {
        if (!strcmp(d->key, "type"))
            data_type = d;
        else if (!strcmp(d->key, "model"))
            data_model = d;
        else if (!strcmp(d->key, "subtype"))
            data_subtype = d;
        else if (!strcmp(d->key, "channel"))
            data_channel = d;
        else if (!strcmp(d->key, "id"))
            data_id = d;
        else if (!strcmp(d->key, "protocol")) // NOTE: needs "-M protocol"
            data_protocol = d;
    }

    // consume entire format string
    while (format && *format) {
        data_t *data_token  = NULL;
        char const *string_token = NULL;
        int leading_slash   = 0;
        char const *t_start = NULL;
        char const *t_end   = NULL;
        char const *d_start = NULL;
        char const *d_end   = NULL;
        // copy until '['
        while (*format && *format != '[')
            *topic++ = *format++;
        // skip '['
        if (!*format)
            break;
        ++format;
        // read slash
        if (*format == '/') {
            leading_slash = 1;
            format++;
        }
        // read key until : or ]
        t_start = t_end = format;
        while (*format && *format != ':' && *format != ']' && *format != '[')
            t_end = ++format;
        // read default until ]
        if (*format == ':') {
            d_start = d_end = ++format;
            while (*format && *format != ']' && *format != '[')
                d_end = ++format;
        }
        // check for proper closing
        if (*format != ']') {
            fprintf(stderr, "%s: unterminated token\n", __func__);
            exit(1);
        }
        ++format;

        // resolve token
        if (!strncmp(t_start, "hostname", t_end - t_start))
            string_token = hostname;
        else if (!strncmp(t_start, "type", t_end - t_start))
            data_token = data_type;
        else if (!strncmp(t_start, "model", t_end - t_start))
            data_token = data_model;
        else if (!strncmp(t_start, "subtype", t_end - t_start))
            data_token = data_subtype;
        else if (!strncmp(t_start, "channel", t_end - t_start))
            data_token = data_channel;
        else if (!strncmp(t_start, "id", t_end - t_start))
            data_token = data_id;
        else if (!strncmp(t_start, "protocol", t_end - t_start))
            data_token = data_protocol;
        else {
            fprintf(stderr, "%s: unknown token \"%.*s\"\n", __func__, (int)(t_end - t_start), t_start);
            exit(1);
        }

        // append token or default
        if (!data_token && !string_token && !d_start)
            continue;
        if (leading_slash)
            *topic++ = '/';
        if (data_token)
            topic = append_topic(topic, data_token);
        else if (string_token)
            topic += sprintf(topic, "%s", string_token);
        else
            topic += sprintf(topic, "%.*s", (int)(d_end - d_start), d_start);
    }

    *topic = '\0';
    return topic;
}

// <prefix>[/type][/model][/subtype][/channel][/id]/battery: "OK"|"LOW"
static void print_mqtt_data(data_output_t *output, data_t *data, char const *format)
{
    data_output_mqtt_t *mqtt = (data_output_mqtt_t *)output;

    char *orig = mqtt->topic + strlen(mqtt->topic); // save current topic
    char *end  = orig;

    // top-level only
    if (!*mqtt->topic) {
        // collect well-known top level keys
        data_t *data_model = NULL;
        for (data_t *d = data; d; d = d->next) {
            if (!strcmp(d->key, "model"))
                data_model = d;
        }

        // "states" topic
        if (!data_model) {
            if (mqtt->states) {
                size_t message_size = 20000; // state message need a large buffer
                char *message       = malloc(message_size);
                if (!message) {
                    WARN_MALLOC("print_mqtt_data()");
                    return; // NOTE: skip output on alloc failure.
                }
                data_print_jsons(data, message, message_size);
                expand_topic(mqtt->topic, mqtt->states, data, mqtt->hostname);
                mqtt_client_publish(mqtt->mgr, mqtt->topic, message);
                *mqtt->topic = '\0'; // clear topic
                free(message);
            }
            return;
        }

        // "events" topic
        if (mqtt->events) {
            char message[1024]; // we expect the biggest strings to be around 500 bytes.
            data_print_jsons(data, message, sizeof(message));
            expand_topic(mqtt->topic, mqtt->events, data, mqtt->hostname);
            mqtt_client_publish(mqtt->mgr, mqtt->topic, message);
            *mqtt->topic = '\0'; // clear topic
        }

        // "devices" topic
        if (!mqtt->devices) {
            return;
        }

        end = expand_topic(mqtt->topic, mqtt->devices, data, mqtt->hostname);
    }

    while (data) {
        if (!strcmp(data->key, "brand")
                || !strcmp(data->key, "type")
                || !strcmp(data->key, "model")
                || !strcmp(data->key, "subtype")) {
            // skip, except "id", "channel"
        }
        else {
            // push topic
            *end = '/';
            strcpy(end + 1, data->key);
            print_value(output, data->type, data->value, data->format);
            *end = '\0'; // pop topic
        }
        data = data->next;
    }
    *orig = '\0'; // restore topic
}

static void print_mqtt_string(data_output_t *output, char const *str, char const *format)
{
    data_output_mqtt_t *mqtt = (data_output_mqtt_t *)output;
    mqtt_client_publish(mqtt->mgr, mqtt->topic, str);
}

static void print_mqtt_double(data_output_t *output, double data, char const *format)
{
    char str[20];
    // use scientific notation for very big/small values
    if (data > 1e7 || data < 1e-4) {
        int ret = snprintf(str, 20, "%g", data);
    }
    else {
        int ret = snprintf(str, 20, "%.5f", data);
        // remove trailing zeros, always keep one digit after the decimal point
        char *p = str + ret - 1;
        while (*p == '0' && p[-1] != '.') {
            *p-- = '\0';
        }
    }

    print_mqtt_string(output, str, format);
}

static void print_mqtt_int(data_output_t *output, int data, char const *format)
{
    char str[20];
    int ret = snprintf(str, 20, "%d", data);
    print_mqtt_string(output, str, format);
}

static void data_output_mqtt_poll(data_output_t *output)
{
    data_output_mqtt_t *mqtt = (data_output_mqtt_t *)output;

    if (!mqtt)
        return;

    mqtt_client_poll(mqtt->mgr);
}

static void data_output_mqtt_free(data_output_t *output)
{
    data_output_mqtt_t *mqtt = (data_output_mqtt_t *)output;

    if (!mqtt)
        return;

    if (mqtt == active_mqtt)
	active_mqtt = NULL;

    free(mqtt->devices);
    free(mqtt->events);
    free(mqtt->states);
    //free(mqtt->homie);
    //free(mqtt->hass);

    mqtt_client_free(mqtt->mgr);
    free(mqtt);
}

static char *mqtt_topic_default(char const *topic, char const *base, char const *suffix)
{
    char path[256];
    char const *p;
    if (topic) {
        p = topic;
    }
    else if (!base) {
        p = suffix;
    }
    else {
        snprintf(path, sizeof(path), "%s/%s", base, suffix);
        p = path;
    }

    char *ret = strdup(p);
    if (!ret)
        WARN_STRDUP("mqtt_topic_default()");
    return ret;
}

struct data_output *data_output_mqtt_create(char const *host, char const *port, char *opts, char const *dev_hint)
{
    data_output_mqtt_t *mqtt = calloc(1, sizeof(data_output_mqtt_t));
    if (!mqtt)
        FATAL_CALLOC("data_output_mqtt_create()");

    gethostname(mqtt->hostname, sizeof(mqtt->hostname) - 1);
    mqtt->hostname[sizeof(mqtt->hostname) - 1] = '\0';
    // only use hostname, not domain part
    char *dot = strchr(mqtt->hostname, '.');
    if (dot)
        *dot = '\0';
    //fprintf(stderr, "Hostname: %s\n", hostname);

    // generate a short deterministic client_id to identify this input device on restart
    uint16_t host_crc = crc16((uint8_t *)mqtt->hostname, strlen(mqtt->hostname), 0x1021, 0xffff);
    uint16_t devq_crc = crc16((uint8_t *)dev_hint, dev_hint ? strlen(dev_hint) : 0, 0x1021, 0xffff);
    char client_id[17];
    snprintf(client_id, sizeof(client_id), "rtl_433-%04x%04x", host_crc, devq_crc);

    // default base topic
    char base_topic[8 + sizeof(mqtt->hostname)];
    snprintf(base_topic, sizeof(base_topic), "rtl_433/%s", mqtt->hostname);

    // default topics
    char const *path_devices = "devices[/type][/model][/subtype][/channel][/id]";
    char const *path_events = "events";
    char const *path_states = "states";

    char *user = NULL;
    char *pass = NULL;
    int retain = 0;

    // parse auth and format options
    char *key, *val;
    while (getkwargs(&opts, &key, &val)) {
        key = remove_ws(key);
        val = trim_ws(val);
        if (!key || !*key)
            continue;
        else if (!strcasecmp(key, "u") || !strcasecmp(key, "user"))
            user = val;
        else if (!strcasecmp(key, "p") || !strcasecmp(key, "pass"))
            pass = val;
        else if (!strcasecmp(key, "r") || !strcasecmp(key, "retain"))
            retain = atobv(val, 1);
        // Simple key-topic mapping
        else if (!strcasecmp(key, "d") || !strcasecmp(key, "devices"))
            mqtt->devices = mqtt_topic_default(val, base_topic, path_devices);
        // deprecated, remove this
        else if (!strcasecmp(key, "c") || !strcasecmp(key, "usechannel")) {
            fprintf(stderr, "\"usechannel=...\" has been removed. Use a topic format string:\n");
            fprintf(stderr, "for \"afterid\"   use e.g. \"devices=rtl_433/[hostname]/devices[/type][/model][/subtype][/id][/channel]\"\n");
            fprintf(stderr, "for \"beforeid\"  use e.g. \"devices=rtl_433/[hostname]/devices[/type][/model][/subtype][/channel][/id]\"\n");
            fprintf(stderr, "for \"replaceid\" use e.g. \"devices=rtl_433/[hostname]/devices[/type][/model][/subtype][/channel]\"\n");
            fprintf(stderr, "for \"no\"        use e.g. \"devices=rtl_433/[hostname]/devices[/type][/model][/subtype][/id]\"\n");
            exit(1);
        }
        // JSON events to single topic
        else if (!strcasecmp(key, "e") || !strcasecmp(key, "events"))
            mqtt->events = mqtt_topic_default(val, base_topic, path_events);
        // JSON states to single topic
        else if (!strcasecmp(key, "s") || !strcasecmp(key, "states"))
            mqtt->states = mqtt_topic_default(val, base_topic, path_states);
        // TODO: Homie Convention https://homieiot.github.io/
        //else if (!strcasecmp(key, "o") || !strcasecmp(key, "homie"))
        //    mqtt->homie = mqtt_topic_default(val, NULL, "homie"); // base topic
        // TODO: Home Assistant MQTT discovery https://www.home-assistant.io/docs/mqtt/discovery/
        //else if (!strcasecmp(key, "a") || !strcasecmp(key, "hass"))
        //    mqtt->hass = mqtt_topic_default(val, NULL, "homeassistant"); // discovery prefix
        else {
            fprintf(stderr, "Invalid key \"%s\" option.\n", key);
            exit(1);
        }
    }

    // Default is to use all formats
    if (!mqtt->devices && !mqtt->events && !mqtt->states) {
        mqtt->devices = mqtt_topic_default(NULL, base_topic, path_devices);
        mqtt->events  = mqtt_topic_default(NULL, base_topic, path_events);
        mqtt->states  = mqtt_topic_default(NULL, base_topic, path_states);
    }
    if (mqtt->devices)
        fprintf(stderr, "Publishing device info to MQTT topic \"%s\".\n", mqtt->devices);
    if (mqtt->events)
        fprintf(stderr, "Publishing events info to MQTT topic \"%s\".\n", mqtt->events);
    if (mqtt->states)
        fprintf(stderr, "Publishing states info to MQTT topic \"%s\".\n", mqtt->states);

    mqtt->output.print_data   = print_mqtt_data;
    mqtt->output.print_array  = print_mqtt_array;
    mqtt->output.print_string = print_mqtt_string;
    mqtt->output.print_double = print_mqtt_double;
    mqtt->output.print_int    = print_mqtt_int;
    mqtt->output.output_poll  = data_output_mqtt_poll;
    mqtt->output.output_free  = data_output_mqtt_free;

    mqtt->mgr = mqtt_client_init(host, port, user, pass, client_id, retain);

    if (!active_mqtt)
	active_mqtt = mqtt;

    return &mqtt->output;
}

static pulse_data_t *active_pulse = NULL;

static void rfraw_received(const struct mg_str *topic, const struct mg_str *payload)
{
    char buf[4096];
    unsigned int bcount, x;
    int n = -1;
    int buckets[8];
    char *p, *start;
    double to_sample;
    bool hl_marking;

    if (!active_pulse)
	return;

    if (sscanf(payload->p, " { \"%*1[Tt]%*1[Ii]%*1[Mm]%*1[Ee]\" : \"%*[^\"]\" , \"%*1[Rr]%*1[Ff]%*1[Rr]%*1[Aa]%*1[Ww]\" : { \"%*1[Dd]%*1[Aa]%*1[Tt]%*1[Aa]\" : \" %*1[Aa]%*1[Aa] %*1[Bb]1 %x %4095[0-9A-Fa-f ] \" } } %n", &bcount, buf, &n) != 2)
	return;
    if ((size_t) n != payload->len)
	return;
    if (bcount < 1 || bcount > 8)
	return;

    to_sample = active_pulse->sample_rate / 1e6;
    p = strtok(buf, " \t\r\n");
    for (size_t i = 0; i < bcount; i++) {
	if (!p) return;
	sscanf(p, "%x", &x);
	buckets[i] = (int)(to_sample * x);
	p = strtok(NULL, " \t\r\n");
    }

    if (!p)
	return;
    start = p;
    p = strtok(NULL, " \t\r\n");
    if (!p || strcmp(p, "55"))
	return;
    if (strtok(NULL, " \t\r\n"))
	return;

    hl_marking = false;
    for (p = start; *p; p++) {
	if (*p >= '8') {
	    hl_marking = true;
	    if (*p >= 'a')
		*p -= 'a' - 'A';
	}
    }
    if ((p - start + 1) / 2 > PD_MAX_PULSES)
	return;

    active_pulse->num_pulses = 0;
    if (hl_marking) {
	hl_marking = true;
	active_pulse->pulse[0] = 0;
	active_pulse->gap[0] = 0;
	for (p = start; *p; p++) {
	    if (hl_marking) {
		if (*p < '8') {
		    hl_marking = false;
		    active_pulse->gap[active_pulse->num_pulses] = buckets[*p - '0'];
		} else {
		    active_pulse->pulse[active_pulse->num_pulses] += buckets[*p - (*p < 'A' ? '8' : 'A' - 2)];
		}
	    } else {
		if (*p < '8') {
		    active_pulse->gap[active_pulse->num_pulses] += buckets[*p - '0'];
		} else {
		    hl_marking = true;
		    active_pulse->pulse[++active_pulse->num_pulses] = buckets[*p - (*p < 'A' ? '8' : 'A' - 2)];
		    active_pulse->gap[active_pulse->num_pulses] = 0;
		}
	    }
	}
	active_pulse->num_pulses++;
    } else {
	for (p = start; p[0] && p[1]; p += 2) {
	    active_pulse->pulse[active_pulse->num_pulses] = buckets[p[0] - '0'];
	    active_pulse->gap[active_pulse->num_pulses] = buckets[p[1] - '0'];
	    active_pulse->num_pulses++;
	}
	if (*p) {
	    active_pulse->pulse[active_pulse->num_pulses] = buckets[*p - '0'];
	    active_pulse->gap[active_pulse->num_pulses] = 0;
	    active_pulse->num_pulses++;
	}
    }
}

const char *input_mqtt_rfraw_config(const char *topic)
{
    if (!active_mqtt)
	return "MQTT input couldn't be enabled without activating MQTT output";

    mqtt_client_subscribe(active_mqtt->mgr, topic, MG_MQTT_QOS(0), rfraw_received);

    return NULL;
}

int input_mqtt_rfraw_read(pulse_data_t *data, uint32_t sample_rate)
{
    if (!active_mqtt)
	return 0;

    pulse_data_clear(data);
    data->sample_rate = sample_rate;
    active_pulse = data;

    do {
	mg_mgr_poll(active_mqtt->mgr, 1000);
    } while (data->num_pulses == 0);

    active_pulse = NULL;

    return 1;
}
