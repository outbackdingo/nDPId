#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/un.h>
#include <netinet/in.h>
#include <ndpi/ndpi_api.h>
#include <ndpi/ndpi_main.h>
#include <ndpi/ndpi_typedefs.h>
#include <pcap/pcap.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>

#if (NDPI_MAJOR == 3 && NDPI_MINOR < 3) || NDPI_MAJOR < 3
#error "nDPI >= 3.3.0 requiired"
#endif

#define MAX_FLOW_ROOTS_PER_THREAD 2048
#define MAX_IDLE_FLOWS_PER_THREAD 64
#define TICK_RESOLUTION 1000
#define MAX_READER_THREADS 4
#define IDLE_SCAN_PERIOD 10000 /* msec */
#define MAX_IDLE_TIME 300000   /* msec */
#define INITIAL_THREAD_HASH 0x03dd018b
#define MAX_PACKETS_PER_FLOW_TO_SEND 15

enum nDPId_l3_type
{
    L3_IP,
    L3_IP6
};

struct nDPId_flow_info
{
    uint32_t flow_id;
    unsigned long long int packets_processed;
    uint64_t first_seen;
    uint64_t last_seen;
    uint64_t hashval;

    enum nDPId_l3_type l3_type;
    union {
        struct
        {
            uint32_t src;
            uint32_t dst;
        } v4;
        struct
        {
            uint64_t src[2];
            uint64_t dst[2];
        } v6;
    } ip_tuple;

    uint16_t min_l4_data_len;
    uint16_t max_l4_data_len;
    unsigned long long int total_l4_data_len;
    uint16_t src_port;
    uint16_t dst_port;

    uint8_t is_midstream_flow : 1;
    uint8_t flow_fin_ack_seen : 1;
    uint8_t flow_ack_seen : 1;
    uint8_t detection_completed : 1;
    uint8_t reserved_00 : 4;
    uint8_t reserved_01[3];
    uint8_t l4_protocol;

    struct ndpi_proto detected_l7_protocol;
    struct ndpi_proto guessed_protocol;

    struct ndpi_flow_struct * ndpi_flow;
    struct ndpi_id_struct * ndpi_src;
    struct ndpi_id_struct * ndpi_dst;
};

struct nDPId_workflow
{
    pcap_t * pcap_handle;

    uint8_t error_or_eof : 1;
    uint8_t reserved_00 : 7;
    uint8_t reserved_01[3];

    unsigned long long int packets_captured;
    unsigned long long int packets_processed;
    unsigned long long int total_l4_data_len;
    unsigned long long int detected_flow_protocols;

    uint64_t last_idle_scan_time;
    uint64_t last_time;

    void ** ndpi_flows_active;
    unsigned long long int max_active_flows;
    unsigned long long int cur_active_flows;
    unsigned long long int total_active_flows;

    void ** ndpi_flows_idle;
    unsigned long long int max_idle_flows;
    unsigned long long int cur_idle_flows;
    unsigned long long int total_idle_flows;

    ndpi_serializer ndpi_serializer;
    struct ndpi_detection_module_struct * ndpi_struct;
};

struct nDPId_reader_thread
{
    struct nDPId_workflow * workflow;
    pthread_t thread_id;
    int json_sockfd;
    int json_sock_reconnect;
    int array_index;
};

enum packet_event
{
    PACKET_EVENT_INVALID = 0,

    PACKET_EVENT_PAYLOAD,
    PACKET_EVENT_PAYLOAD_FLOW,

    PACKET_EVENT_COUNT
};

enum flow_event
{
    FLOW_EVENT_INVALID = 0,

    FLOW_EVENT_NEW,
    FLOW_EVENT_END,
    FLOW_EVENT_IDLE,
    FLOW_EVENT_GUESSED,
    FLOW_EVENT_DETECTED,
    FLOW_EVENT_NOT_DETECTED,

    FLOW_EVENT_COUNT
};
enum basic_event
{
    BASIC_EVENT_INVALID = 0,

    UNKNOWN_DATALINK_LAYER,
    UNKNOWN_L3_PROTOCOL,
    NON_IP_PACKET,
    ETHERNET_PACKET_TOO_SHORT,
    ETHERNET_PACKET_UNKNOWN,
    IP4_PACKET_TOO_SHORT,
    IP4_SIZE_SMALLER_THAN_HEADER,
    IP4_L4_PAYLOAD_DETECTION_FAILED,
    IP6_PACKET_TOO_SHORT,
    IP6_SIZE_SMALLER_THAN_HEADER,
    IP6_L4_PAYLOAD_DETECTION_FAILED,
    TCP_PACKET_TOO_SHORT,
    UDP_PACKET_TOO_SHORT,
    CAPTURE_SIZE_SMALLER_THAN_PACKET_SIZE,
    MAX_FLOW_TO_TRACK,
    FLOW_MEMORY_ALLOCATION_FAILED,
    NDPI_FLOW_MEMORY_ALLOCATION_FAILED,
    NDPI_ID_MEMORY_ALLOCATION_FAILED,

    BASIC_EVENT_COUNT
};

static char const * const packet_event_name_table[PACKET_EVENT_COUNT] = {[PACKET_EVENT_INVALID] = "invalid",
                                                                         [PACKET_EVENT_PAYLOAD] = "packet",
                                                                         [PACKET_EVENT_PAYLOAD_FLOW] = "packet-flow"};

static char const * const flow_event_name_table[FLOW_EVENT_COUNT] = {[FLOW_EVENT_INVALID] = "invalid",
                                                                     [FLOW_EVENT_NEW] = "new",
                                                                     [FLOW_EVENT_END] = "end",
                                                                     [FLOW_EVENT_IDLE] = "idle",
                                                                     [FLOW_EVENT_GUESSED] = "guessed",
                                                                     [FLOW_EVENT_DETECTED] = "detected",
                                                                     [FLOW_EVENT_NOT_DETECTED] = "not-detected"};
static char const * const basic_event_name_table[BASIC_EVENT_COUNT] = {
    [BASIC_EVENT_INVALID] = "invalid",
    [UNKNOWN_DATALINK_LAYER] = "Unknown datalink layer packet",
    [UNKNOWN_L3_PROTOCOL] = "Unknown L3 protocol",
    [NON_IP_PACKET] = "Non IP packet",
    [ETHERNET_PACKET_TOO_SHORT] = "Ethernet packet too short",
    [ETHERNET_PACKET_UNKNOWN] = "Unknown Ethernet packet type",
    [IP4_PACKET_TOO_SHORT] = "IP4 packet too short",
    [IP4_SIZE_SMALLER_THAN_HEADER] = "Packet smaller than IP4 header",
    [IP4_L4_PAYLOAD_DETECTION_FAILED] = "nDPI IPv4/L4 payload detection failed",
    [IP6_PACKET_TOO_SHORT] = "IP6 packet too short",
    [IP6_SIZE_SMALLER_THAN_HEADER] = "Packet smaller than IP6 header",
    [IP6_L4_PAYLOAD_DETECTION_FAILED] = "nDPI IPv6/L4 payload detection failed",
    [TCP_PACKET_TOO_SHORT] = "TCP packet smaller than expected",
    [UDP_PACKET_TOO_SHORT] = "UDP packet smaller than expected",
    [CAPTURE_SIZE_SMALLER_THAN_PACKET_SIZE] = "Captured packet size is smaller than packet size",
    [MAX_FLOW_TO_TRACK] = "Max flows to track reached",
    [FLOW_MEMORY_ALLOCATION_FAILED] = "Flow memory allocation failed",
    [NDPI_FLOW_MEMORY_ALLOCATION_FAILED] = "nDPI Flow memory allocation failed",
    [NDPI_ID_MEMORY_ALLOCATION_FAILED] = "Not enough memory for src id struct",
};
static struct nDPId_reader_thread reader_threads[MAX_READER_THREADS] = {};
static int reader_thread_count = MAX_READER_THREADS;
static int main_thread_shutdown = 0;
static uint32_t global_flow_id = 0;

static char * pcap_file_or_interface = NULL;
static int log_to_stderr = 0;
static char json_sockpath[UNIX_PATH_MAX] = "/tmp/ndpid-collector.sock";

static void free_workflow(struct nDPId_workflow ** const workflow);
static void serialize_and_send(struct nDPId_reader_thread * const reader_thread);
static void jsonize_flow_event(struct nDPId_reader_thread * const reader_thread,
                               struct nDPId_flow_info const * const flow,
                               enum flow_event event);

static struct nDPId_workflow * init_workflow(char const * const file_or_device)
{
    char pcap_error_buffer[PCAP_ERRBUF_SIZE];
    struct nDPId_workflow * workflow = (struct nDPId_workflow *)ndpi_calloc(1, sizeof(*workflow));

    if (workflow == NULL)
    {
        return NULL;
    }

    if (access(file_or_device, R_OK) != 0 && errno == ENOENT)
    {
        workflow->pcap_handle = pcap_open_live(file_or_device, 65535, 1, 250, pcap_error_buffer);
    }
    else
    {
        workflow->pcap_handle =
            pcap_open_offline_with_tstamp_precision(file_or_device, PCAP_TSTAMP_PRECISION_MICRO, pcap_error_buffer);
    }

    if (workflow->pcap_handle == NULL)
    {
        syslog(LOG_DAEMON | LOG_ERR,
               "pcap_open_live / pcap_open_offline_with_tstamp_precision: %s\n",
               pcap_error_buffer);
        free_workflow(&workflow);
        return NULL;
    }

    ndpi_init_prefs init_prefs = ndpi_no_prefs;
    workflow->ndpi_struct = ndpi_init_detection_module(init_prefs);
    if (workflow->ndpi_struct == NULL)
    {
        free_workflow(&workflow);
        return NULL;
    }

    workflow->total_active_flows = 0;
    workflow->max_active_flows = MAX_FLOW_ROOTS_PER_THREAD;
    workflow->ndpi_flows_active = (void **)ndpi_calloc(workflow->max_active_flows, sizeof(void *));
    if (workflow->ndpi_flows_active == NULL)
    {
        free_workflow(&workflow);
        return NULL;
    }

    workflow->total_idle_flows = 0;
    workflow->max_idle_flows = MAX_IDLE_FLOWS_PER_THREAD;
    workflow->ndpi_flows_idle = (void **)ndpi_calloc(workflow->max_idle_flows, sizeof(void *));
    if (workflow->ndpi_flows_idle == NULL)
    {
        free_workflow(&workflow);
        return NULL;
    }

    NDPI_PROTOCOL_BITMASK protos;
    NDPI_BITMASK_SET_ALL(protos);
    ndpi_set_protocol_detection_bitmask2(workflow->ndpi_struct, &protos);
    ndpi_finalize_initalization(workflow->ndpi_struct);

    if (ndpi_init_serializer_ll(&workflow->ndpi_serializer, ndpi_serialization_format_json, BUFSIZ) != 1)
    {
        return NULL;
    }

    return workflow;
}

static void ndpi_flow_info_freer(void * const node)
{
    struct nDPId_flow_info * const flow = (struct nDPId_flow_info *)node;

    ndpi_free(flow->ndpi_dst);
    ndpi_free(flow->ndpi_src);
    ndpi_flow_free(flow->ndpi_flow);
    ndpi_free(flow);
}

static void free_workflow(struct nDPId_workflow ** const workflow)
{
    struct nDPId_workflow * const w = *workflow;

    if (w == NULL)
    {
        return;
    }

    if (w->pcap_handle != NULL)
    {
        pcap_close(w->pcap_handle);
        w->pcap_handle = NULL;
    }

    if (w->ndpi_struct != NULL)
    {
        ndpi_exit_detection_module(w->ndpi_struct);
    }
    for (size_t i = 0; i < w->max_active_flows; i++)
    {
        ndpi_tdestroy(w->ndpi_flows_active[i], ndpi_flow_info_freer);
    }
    ndpi_free(w->ndpi_flows_active);
    ndpi_free(w->ndpi_flows_idle);
    ndpi_term_serializer(&w->ndpi_serializer);
    ndpi_free(w);
    *workflow = NULL;
}

static int setup_reader_threads(char const * const file_or_device)
{
    char const * file_or_default_device;
    char pcap_error_buffer[PCAP_ERRBUF_SIZE];

    if (reader_thread_count > MAX_READER_THREADS)
    {
        return 1;
    }

    if (file_or_device == NULL)
    {
        file_or_default_device = pcap_lookupdev(pcap_error_buffer);
        if (file_or_default_device == NULL)
        {
            syslog(LOG_DAEMON | LOG_ERR, "pcap_lookupdev: %s\n", pcap_error_buffer);
            return 1;
        }
    }
    else
    {
        file_or_default_device = file_or_device;
    }

    for (int i = 0; i < reader_thread_count; ++i)
    {
        reader_threads[i].workflow = init_workflow(file_or_default_device);
        if (reader_threads[i].workflow == NULL)
        {
            return 1;
        }
    }

    return 0;
}

static int ip_tuple_to_string(struct nDPId_flow_info const * const flow,
                              char * const src_addr_str,
                              size_t src_addr_len,
                              char * const dst_addr_str,
                              size_t dst_addr_len)
{
    switch (flow->l3_type)
    {
        case L3_IP:
            return inet_ntop(AF_INET, (struct sockaddr_in *)&flow->ip_tuple.v4.src, src_addr_str, src_addr_len) !=
                       NULL &&
                   inet_ntop(AF_INET, (struct sockaddr_in *)&flow->ip_tuple.v4.dst, dst_addr_str, dst_addr_len) != NULL;
        case L3_IP6:
            return inet_ntop(AF_INET6, (struct sockaddr_in6 *)&flow->ip_tuple.v6.src[0], src_addr_str, src_addr_len) !=
                       NULL &&
                   inet_ntop(AF_INET6, (struct sockaddr_in6 *)&flow->ip_tuple.v6.dst[0], dst_addr_str, dst_addr_len) !=
                       NULL;
    }

    return 0;
}

static int ip_tuples_equal(struct nDPId_flow_info const * const A, struct nDPId_flow_info const * const B)
{
    // generate a warning if the enum changes
    switch (A->l3_type)
    {
        case L3_IP:
        case L3_IP6:
            break;
    }
    if (A->l3_type == L3_IP && B->l3_type == L3_IP6)
    {
        return A->ip_tuple.v4.src == B->ip_tuple.v4.src && A->ip_tuple.v4.dst == B->ip_tuple.v4.dst;
    }
    else if (A->l3_type == L3_IP6 && B->l3_type == L3_IP6)
    {
        return A->ip_tuple.v6.src[0] == B->ip_tuple.v6.src[0] && A->ip_tuple.v6.src[1] == B->ip_tuple.v6.src[1] &&
               A->ip_tuple.v6.dst[0] == B->ip_tuple.v6.dst[0] && A->ip_tuple.v6.dst[1] == B->ip_tuple.v6.dst[1];
    }
    return 0;
}

static int ip_tuples_compare(struct nDPId_flow_info const * const A, struct nDPId_flow_info const * const B)
{
    // generate a warning if the enum changes
    switch (A->l3_type)
    {
        case L3_IP:
        case L3_IP6:
            break;
    }
    if (A->l3_type == L3_IP && B->l3_type == L3_IP6)
    {
        if (A->ip_tuple.v4.src < B->ip_tuple.v4.src || A->ip_tuple.v4.dst < B->ip_tuple.v4.dst)
        {
            return -1;
        }
        if (A->ip_tuple.v4.src > B->ip_tuple.v4.src || A->ip_tuple.v4.dst > B->ip_tuple.v4.dst)
        {
            return 1;
        }
    }
    else if (A->l3_type == L3_IP6 && B->l3_type == L3_IP6)
    {
        if ((A->ip_tuple.v6.src[0] < B->ip_tuple.v6.src[0] && A->ip_tuple.v6.src[1] < B->ip_tuple.v6.src[1]) ||
            (A->ip_tuple.v6.dst[0] < B->ip_tuple.v6.dst[0] && A->ip_tuple.v6.dst[1] < B->ip_tuple.v6.dst[1]))
        {
            return -1;
        }
        if ((A->ip_tuple.v6.src[0] > B->ip_tuple.v6.src[0] && A->ip_tuple.v6.src[1] > B->ip_tuple.v6.src[1]) ||
            (A->ip_tuple.v6.dst[0] > B->ip_tuple.v6.dst[0] && A->ip_tuple.v6.dst[1] > B->ip_tuple.v6.dst[1]))
        {
            return 1;
        }
    }
    if (A->src_port < B->src_port || A->dst_port < B->dst_port)
    {
        return -1;
    }
    else if (A->src_port > B->src_port || A->dst_port > B->dst_port)
    {
        return 1;
    }
    return 0;
}

static void ndpi_idle_scan_walker(void const * const A, ndpi_VISIT which, int depth, void * const user_data)
{
    struct nDPId_workflow * const workflow = (struct nDPId_workflow *)user_data;
    struct nDPId_flow_info * const flow = *(struct nDPId_flow_info **)A;

    (void)depth;

    if (workflow == NULL || flow == NULL)
    {
        return;
    }

    if (workflow->cur_idle_flows == MAX_IDLE_FLOWS_PER_THREAD)
    {
        return;
    }

    if (which == ndpi_preorder || which == ndpi_leaf)
    {
        if ((flow->flow_fin_ack_seen == 1 && flow->flow_ack_seen == 1) ||
            flow->last_seen + MAX_IDLE_TIME < workflow->last_time)
        {
            char src_addr_str[INET6_ADDRSTRLEN + 1];
            char dst_addr_str[INET6_ADDRSTRLEN + 1];
            ip_tuple_to_string(flow, src_addr_str, sizeof(src_addr_str), dst_addr_str, sizeof(dst_addr_str));
            workflow->ndpi_flows_idle[workflow->cur_idle_flows++] = flow;
            workflow->total_idle_flows++;
        }
    }
}

static int ndpi_workflow_node_cmp(void const * const A, void const * const B)
{
    struct nDPId_flow_info const * const flow_info_a = (struct nDPId_flow_info *)A;
    struct nDPId_flow_info const * const flow_info_b = (struct nDPId_flow_info *)B;

    if (flow_info_a->hashval < flow_info_b->hashval)
    {
        return (-1);
    }
    else if (flow_info_a->hashval > flow_info_b->hashval)
    {
        return (1);
    }

    /* Flows have the same hash */
    if (flow_info_a->l4_protocol < flow_info_b->l4_protocol)
    {
        return (-1);
    }
    else if (flow_info_a->l4_protocol > flow_info_b->l4_protocol)
    {
        return (1);
    }

    if (ip_tuples_equal(flow_info_a, flow_info_b) != 0 && flow_info_a->src_port == flow_info_b->src_port &&
        flow_info_a->dst_port == flow_info_b->dst_port)
    {
        return (0);
    }

    return ip_tuples_compare(flow_info_a, flow_info_b);
}

static void check_for_idle_flows(struct nDPId_reader_thread * const reader_thread)
{
    struct nDPId_workflow * const workflow = reader_thread->workflow;

    if (workflow->last_idle_scan_time + IDLE_SCAN_PERIOD < workflow->last_time)
    {
        for (size_t idle_scan_index = 0; idle_scan_index < workflow->max_active_flows; ++idle_scan_index)
        {
            ndpi_twalk(workflow->ndpi_flows_active[idle_scan_index], ndpi_idle_scan_walker, workflow);

            while (workflow->cur_idle_flows > 0)
            {
                struct nDPId_flow_info * const f =
                    (struct nDPId_flow_info *)workflow->ndpi_flows_idle[--workflow->cur_idle_flows];
                jsonize_flow_event(reader_thread, f, FLOW_EVENT_IDLE);
                ndpi_tdelete(f, &workflow->ndpi_flows_active[idle_scan_index], ndpi_workflow_node_cmp);
                ndpi_flow_info_freer(f);
                workflow->cur_active_flows--;
            }
        }

        workflow->last_idle_scan_time = workflow->last_time;
    }
}

static int jsonize_l3_l4_dpi(struct nDPId_workflow * const workflow, struct nDPId_flow_info const * const flow)
{
    ndpi_serializer * const serializer = &workflow->ndpi_serializer;
    char src_name[32] = {};
    char dst_name[32] = {};

    switch (flow->l3_type)
    {
        case L3_IP:
            ndpi_serialize_string_string(serializer, "l3_proto", "ip4");
            inet_ntop(AF_INET, &flow->ip_tuple.v4.src, src_name, sizeof(src_name));
            inet_ntop(AF_INET, &flow->ip_tuple.v4.dst, dst_name, sizeof(dst_name));
            break;
        case L3_IP6:
            ndpi_serialize_string_string(serializer, "l3_proto", "ip6");
            inet_ntop(AF_INET6, &flow->ip_tuple.v6.src[0], src_name, sizeof(src_name));
            inet_ntop(AF_INET6, &flow->ip_tuple.v6.dst[0], dst_name, sizeof(dst_name));
            /* For consistency across platforms replace :0: with :: */
            ndpi_patchIPv6Address(src_name), ndpi_patchIPv6Address(dst_name);
            break;
        default:
            ndpi_serialize_string_string(serializer, "l3_proto", "unknown");
    }

    ndpi_serialize_string_string(serializer, "src_ip", src_name);
    ndpi_serialize_string_string(serializer, "dest_ip", dst_name);
    if (flow->src_port)
    {
        ndpi_serialize_string_uint32(serializer, "src_port", flow->src_port);
    }
    if (flow->dst_port)
    {
        ndpi_serialize_string_uint32(serializer, "dst_port", flow->dst_port);
    }

    switch (flow->l4_protocol)
    {
        case IPPROTO_TCP:
            ndpi_serialize_string_string(serializer, "l4_proto", "tcp");
            break;
        case IPPROTO_UDP:
            ndpi_serialize_string_string(serializer, "l4_proto", "udp");
            break;
        case IPPROTO_ICMP:
            ndpi_serialize_string_string(serializer, "l4_proto", "icmp");
            break;
        case IPPROTO_ICMPV6:
            ndpi_serialize_string_string(serializer, "l4_proto", "icmp6");
            break;
        default:
            ndpi_serialize_string_uint32(serializer, "l4_proto", flow->l4_protocol);
            break;
    }

    return ndpi_dpi2json(workflow->ndpi_struct, flow->ndpi_flow, flow->detected_l7_protocol, serializer);
}

static void jsonize_basic(struct nDPId_reader_thread * const reader_thread)
{
    struct nDPId_workflow * const workflow = reader_thread->workflow;

    ndpi_serialize_string_int32(&workflow->ndpi_serializer, "thread_id", reader_thread->array_index);
    ndpi_serialize_string_uint32(&workflow->ndpi_serializer, "packet_id", workflow->packets_captured);
}

static void jsonize_flow(struct nDPId_workflow * const workflow, struct nDPId_flow_info const * const flow)
{
    ndpi_serialize_string_uint32(&workflow->ndpi_serializer, "flow_id", flow->flow_id);
    ndpi_serialize_string_uint64(&workflow->ndpi_serializer, "flow_packet_id", flow->packets_processed);
    ndpi_serialize_string_uint64(&workflow->ndpi_serializer, "flow_first_seen", flow->first_seen);
    ndpi_serialize_string_uint64(&workflow->ndpi_serializer, "flow_last_seen", flow->last_seen);
    ndpi_serialize_string_uint64(&workflow->ndpi_serializer, "flow_tot_l4_data_len", flow->total_l4_data_len);
    ndpi_serialize_string_uint64(&workflow->ndpi_serializer, "flow_min_l4_data_len", flow->min_l4_data_len);
    ndpi_serialize_string_uint64(&workflow->ndpi_serializer, "flow_max_l4_data_len", flow->max_l4_data_len);
    ndpi_serialize_string_uint64(&workflow->ndpi_serializer,
                                 "flow_avg_l4_data_len",
                                 (flow->packets_processed > 0 ? flow->total_l4_data_len / flow->packets_processed : 0));
    ndpi_serialize_string_uint32(&workflow->ndpi_serializer, "midstream", flow->is_midstream_flow);
    ndpi_serialize_risk(&workflow->ndpi_serializer, flow->ndpi_flow);

    if (jsonize_l3_l4_dpi(workflow, flow) != 0)
    {
        syslog(LOG_DAEMON | LOG_ERR,
               "[%8llu, %4u] flow2json/dpi2json failed\n",
               workflow->packets_captured,
               flow->flow_id);
    }
}

static int connect_to_json_socket(struct nDPId_reader_thread * const reader_thread)
{
    struct nDPId_workflow * const workflow = reader_thread->workflow;
    struct sockaddr_un saddr;

    close(reader_thread->json_sockfd);

    reader_thread->json_sockfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (reader_thread->json_sockfd < 0)
    {
        reader_thread->json_sock_reconnect = 1;
        return 1;
    }

    saddr.sun_family = AF_UNIX;
    if (snprintf(saddr.sun_path, sizeof(saddr.sun_path), "%s", json_sockpath) < 0 ||
        connect(reader_thread->json_sockfd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0)
    {
        reader_thread->json_sock_reconnect = 1;
        return 1;
    }

    if (shutdown(reader_thread->json_sockfd, SHUT_RD) != 0)
    {
        return 1;
    }

    if (fcntl(reader_thread->json_sockfd, F_SETFL, fcntl(reader_thread->json_sockfd, F_GETFL, 0) | O_NONBLOCK) == -1)
    {
        reader_thread->json_sock_reconnect = 1;
        return 1;
    }

    reader_thread->json_sock_reconnect = 0;

    if (ndpi_serialize_string_int32(&workflow->ndpi_serializer, "thread_id", reader_thread->array_index) != 0 ||
        ndpi_serialize_string_boolean(&workflow->ndpi_serializer, "init_complete", 1) != 0)
    {
        syslog(LOG_DAEMON | LOG_ERR,
               "[%8llu, %d] JSON serialize buffer failed",
               reader_thread->workflow->packets_captured,
               reader_thread->array_index);
    }
    serialize_and_send(reader_thread);

    return 0;
}

static void send_to_json_sink(struct nDPId_reader_thread * const reader_thread,
                              char const * const json_str,
                              size_t json_str_len)
{
    struct nDPId_workflow * const workflow = reader_thread->workflow;
    int saved_errno;
    int s_ret;
    char newline_json_str[BUFSIZ];

    s_ret = snprintf(newline_json_str, sizeof(newline_json_str), "%zu%.*s\n",
                     json_str_len, (int)json_str_len, json_str);
    if (s_ret < 0 || s_ret > (int)sizeof(newline_json_str))
    {
        syslog(LOG_DAEMON | LOG_ERR,
               "[%8llu, %d] JSON buffer prepare failed",
               workflow->packets_captured,
               reader_thread->array_index);
        return;
    }

    if (reader_thread->json_sock_reconnect != 0)
    {
        if (connect_to_json_socket(reader_thread) == 0)
        {
            syslog(LOG_DAEMON | LOG_ERR,
                   "[%8llu, %d] Reconnected to JSON sink",
                   workflow->packets_captured,
                   reader_thread->array_index);
        }
    }

    if (reader_thread->json_sock_reconnect == 0 &&
        send(reader_thread->json_sockfd, newline_json_str, s_ret, MSG_NOSIGNAL) < 0)
    {
        saved_errno = errno;
        syslog(LOG_DAEMON | LOG_ERR,
               "[%8llu, %d] send data to JSON sink failed: %s",
               workflow->packets_captured,
               reader_thread->array_index,
               strerror(saved_errno));
        if (saved_errno == EPIPE)
        {
            syslog(LOG_DAEMON | LOG_ERR,
                   "[%8llu, %d] Lost connection to JSON sink",
                   workflow->packets_captured,
                   reader_thread->array_index);
        }
        reader_thread->json_sock_reconnect = 1;
    }
}

static void serialize_and_send(struct nDPId_reader_thread * const reader_thread)
{
    char * json_str;
    uint32_t json_str_len;

    json_str = ndpi_serializer_get_buffer(&reader_thread->workflow->ndpi_serializer, &json_str_len);
    if (json_str == NULL || json_str_len == 0)
    {

        syslog(LOG_DAEMON | LOG_ERR,
               "[%8llu, %d] jsonize failed, buffer length: %u\n",
               reader_thread->workflow->packets_captured,
               reader_thread->array_index,
               json_str_len);
    }
    else
    {

        send_to_json_sink(reader_thread, json_str, json_str_len);
    }
    ndpi_reset_serializer(&reader_thread->workflow->ndpi_serializer);
}

size_t base64_out_len(size_t in_len)
{
    return ((in_len + 2) / 3) * 4;
}

char * base64_encode(uint8_t const * in, size_t in_len, char * const out, size_t const out_len)
{
    static const unsigned char base64_table[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = 0, ret_size;
    int i = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    ret_size = base64_out_len(in_len);
    if (out_len < ret_size)
    {
        return NULL;
    }

    while (in_len-- != 0)
    {
        char_array_3[i++] = *(in++);
        if (i == 3)
        {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (i = 0; i < 4; i++)
            {
                out[len++] = base64_table[char_array_4[i]];
            }
            i = 0;
        }
    }

    if (i != 0)
    {
        for (int j = i; j < 3; j++)
        {
            char_array_3[j] = '\0';
        }

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (int j = 0; (j < i + 1); j++)
        {
            out[len++] = base64_table[char_array_4[j]];
        }

        while ((i++ < 3))
        {
            out[len++] = '=';
        }
    }

    out[len++] = '\0';

    return out;
}

static void jsonize_packet_event(struct nDPId_reader_thread * const reader_thread,
                                 struct pcap_pkthdr const * const header,
                                 uint8_t const * const packet,
                                 struct nDPId_flow_info const * const flow,
                                 enum packet_event event)
{
    struct nDPId_workflow * const workflow = reader_thread->workflow;
    char const ev[] = "packet_event_name";

    if (event == PACKET_EVENT_PAYLOAD_FLOW)
    {
        if (flow == NULL)
        {
            syslog(LOG_DAEMON | LOG_ERR,
                   "[%8llu, %d] BUG: got a PACKET_EVENT_PAYLOAD_FLOW with a flow pointer equals NULL",
                   reader_thread->workflow->packets_captured,
                   reader_thread->array_index);
            return;
        }
        if (flow->packets_processed > MAX_PACKETS_PER_FLOW_TO_SEND)
        {
            return;
        }
        ndpi_serialize_string_uint32(&workflow->ndpi_serializer, "flow_id", flow->flow_id);
        ndpi_serialize_string_uint64(&workflow->ndpi_serializer, "flow_packet_id", flow->packets_processed);
        ndpi_serialize_string_uint32(&workflow->ndpi_serializer, "max_packets", MAX_PACKETS_PER_FLOW_TO_SEND);
    }

    ndpi_serialize_string_int32(&workflow->ndpi_serializer, "packet_event_id", event);
    if (event > PACKET_EVENT_INVALID && event < PACKET_EVENT_COUNT)
    {
        ndpi_serialize_string_string(&workflow->ndpi_serializer, ev, packet_event_name_table[event]);
    }
    else
    {
        ndpi_serialize_string_string(&workflow->ndpi_serializer, ev, packet_event_name_table[PACKET_EVENT_INVALID]);
    }

    jsonize_basic(reader_thread);

    size_t base64_data_len = base64_out_len(header->caplen);
    char base64_data[BUFSIZ];
    if (ndpi_serialize_string_boolean(&workflow->ndpi_serializer,
                                      "pkt_oversize",
                                      base64_data_len > sizeof(base64_data)) != 0 ||
        ndpi_serialize_string_uint64(&workflow->ndpi_serializer, "pkt_ts", header->ts.tv_sec) != 0 ||
        ndpi_serialize_string_uint32(&workflow->ndpi_serializer, "pkt_len", header->len) != 0 ||
        ndpi_serialize_string_string(&workflow->ndpi_serializer,
                                     "pkt",
                                     base64_encode(packet, header->caplen, base64_data, sizeof(base64_data))) != 0 ||
        ndpi_serialize_string_uint32(&workflow->ndpi_serializer, "pkt_caplen", header->caplen) != 0)
    {
        syslog(LOG_DAEMON | LOG_ERR,
               "[%8llu, %d] JSON serialize buffer failed",
               reader_thread->workflow->packets_captured,
               reader_thread->array_index);
    }
    serialize_and_send(reader_thread);
}

static void jsonize_flow_event(struct nDPId_reader_thread * const reader_thread,
                               struct nDPId_flow_info const * const flow,
                               enum flow_event event)
{
    struct nDPId_workflow * const workflow = reader_thread->workflow;
    char const ev[] = "flow_event_name";

    ndpi_serialize_string_int32(&workflow->ndpi_serializer, "flow_event_id", event);
    if (event > FLOW_EVENT_INVALID && event < FLOW_EVENT_COUNT)
    {
        ndpi_serialize_string_string(&workflow->ndpi_serializer, ev, flow_event_name_table[event]);
    }
    else
    {
        ndpi_serialize_string_string(&workflow->ndpi_serializer, ev, flow_event_name_table[FLOW_EVENT_INVALID]);
    }
    jsonize_basic(reader_thread);
    jsonize_flow(workflow, flow);
    serialize_and_send(reader_thread);
}

static void jsonize_format_error(struct nDPId_reader_thread * const reader_thread, uint32_t format_index)
{
    ndpi_serialize_string_string(&reader_thread->workflow->ndpi_serializer, "serializer-error", "format");
    ndpi_serialize_string_uint32(&reader_thread->workflow->ndpi_serializer, "serializer-format-index", format_index);
    serialize_and_send(reader_thread);
}

static void vjsonize_basic_eventf(struct nDPId_reader_thread * const reader_thread, char const * format, va_list ap)
{
    uint8_t got_jsonkey = 0;
    uint8_t is_long_long = 0;
    char json_key[BUFSIZ];
    uint32_t format_index = 0;

    while (*format)
    {
        if (got_jsonkey == 0)
        {
            json_key[0] = '\0';
        }

        switch (*format++)
        {
            case 's':
            {
                format_index++;
                char * value = va_arg(ap, char *);
                if (got_jsonkey == 0)
                {
                    snprintf(json_key, sizeof(json_key), "%s", value);
                    got_jsonkey = 1;
                }
                else
                {
                    ndpi_serialize_string_string(&reader_thread->workflow->ndpi_serializer, json_key, value);
                    got_jsonkey = 0;
                }
                break;
            }
            case 'f':
            {
                format_index++;
                if (got_jsonkey == 1)
                {
                    float value = va_arg(ap, double);
                    ndpi_serialize_string_float(&reader_thread->workflow->ndpi_serializer, json_key, value, "%.2f");
                    got_jsonkey = 0;
                }
                else
                {
                    jsonize_format_error(reader_thread, format_index);
                    return;
                }
                break;
            }
            case 'z':
            case 'l':
                format_index++;
                if (got_jsonkey != 1)
                {
                    jsonize_format_error(reader_thread, format_index);
                    return;
                }
                if (*format == 'l')
                {
                    format++;
                    is_long_long = 1;
                }
                else
                {
                    is_long_long = 0;
                }
                if (*format == 'd')
                {
                    long long int value;
                    if (is_long_long != 0)
                    {
                        value = va_arg(ap, long long int);
                    }
                    else
                    {
                        value = va_arg(ap, long int);
                    }
                    ndpi_serialize_string_int64(&reader_thread->workflow->ndpi_serializer, json_key, value);
                    got_jsonkey = 0;
                }
                else if (*format == 'u')
                {
                    unsigned long long int value;
                    if (is_long_long != 0)
                    {
                        value = va_arg(ap, unsigned long long int);
                    }
                    else
                    {
                        value = va_arg(ap, unsigned long int);
                    }
                    ndpi_serialize_string_uint64(&reader_thread->workflow->ndpi_serializer, json_key, value);
                    got_jsonkey = 0;
                }
                else
                {
                    jsonize_format_error(reader_thread, format_index);
                    return;
                }
                format++;
                break;
            case 'u':
                format_index++;
                if (got_jsonkey == 1)
                {
                    unsigned int value = va_arg(ap, unsigned int);
                    ndpi_serialize_string_uint32(&reader_thread->workflow->ndpi_serializer, json_key, value);
                    got_jsonkey = 0;
                }
                else
                {
                    jsonize_format_error(reader_thread, format_index);
                    return;
                }
                break;
            case 'd':
                format_index++;
                if (got_jsonkey == 1)
                {
                    int value = va_arg(ap, int);
                    ndpi_serialize_string_int32(&reader_thread->workflow->ndpi_serializer, json_key, value);
                    got_jsonkey = 0;
                }
                else
                {
                    jsonize_format_error(reader_thread, format_index);
                    return;
                }
                break;
            case ' ':
            case ',':
            case '%':
                break;
            default:
                jsonize_format_error(reader_thread, format_index);
                return;
        }
    }
}

__attribute__((format(printf, 3, 4))) static void jsonize_basic_eventf(struct nDPId_reader_thread * const reader_thread,
                                                                       enum basic_event event,
                                                                       char const * format,
                                                                       ...)
{
    struct nDPId_workflow * const workflow = reader_thread->workflow;
    va_list ap;
    char const ev[] = "basic_event_name";

    ndpi_serialize_string_int32(&reader_thread->workflow->ndpi_serializer, "basic_event_id", event);
    if (event > BASIC_EVENT_INVALID && event < BASIC_EVENT_COUNT)
    {
        ndpi_serialize_string_string(&workflow->ndpi_serializer, ev, basic_event_name_table[event]);
    }
    else
    {
        ndpi_serialize_string_string(&workflow->ndpi_serializer, ev, basic_event_name_table[BASIC_EVENT_INVALID]);
    }
    jsonize_basic(reader_thread);

    if (format != NULL)
    {
        va_start(ap, format);
        vjsonize_basic_eventf(reader_thread, format, ap);
        va_end(ap);
    }

    serialize_and_send(reader_thread);
}

static void ndpi_process_packet(uint8_t * const args,
                                struct pcap_pkthdr const * const header,
                                uint8_t const * const packet)
{
    struct nDPId_reader_thread * const reader_thread = (struct nDPId_reader_thread *)args;
    struct nDPId_workflow * workflow;
    struct nDPId_flow_info flow = {};

    size_t hashed_index;
    void * tree_result;
    struct nDPId_flow_info * flow_to_process;

    uint8_t direction_changed = 0;
    uint8_t is_new_flow = 0;
    struct ndpi_id_struct * ndpi_src;
    struct ndpi_id_struct * ndpi_dst;

    const struct ndpi_ethhdr * ethernet;
    const struct ndpi_iphdr * ip;
    struct ndpi_ipv6hdr * ip6;

    uint64_t time_ms;
    const uint16_t eth_offset = 0;
    uint16_t ip_offset;
    uint16_t ip_size;

    const uint8_t * l4_ptr = NULL;
    uint16_t l4_len = 0;

    uint16_t type;
    int thread_index = INITIAL_THREAD_HASH; // generated with `dd if=/dev/random bs=1024 count=1 |& hd'

    if (reader_thread == NULL)
    {
        return;
    }
    workflow = reader_thread->workflow;

    if (workflow == NULL)
    {
        return;
    }

    workflow->packets_captured++;
    time_ms = ((uint64_t)header->ts.tv_sec) * TICK_RESOLUTION + header->ts.tv_usec / (1000000 / TICK_RESOLUTION);
    workflow->last_time = time_ms;

    check_for_idle_flows(reader_thread);

    /* process datalink layer */
    switch (pcap_datalink(workflow->pcap_handle))
    {
        case DLT_NULL:
            if (ntohl(*((uint32_t *)&packet[eth_offset])) == 0x00000002)
            {
                type = ETH_P_IP;
            }
            else
            {
                type = ETH_P_IPV6;
            }
            ip_offset = 4 + eth_offset;
            break;
        case DLT_EN10MB:
            if (header->len < sizeof(struct ndpi_ethhdr))
            {
                jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
                jsonize_basic_eventf(reader_thread, ETHERNET_PACKET_TOO_SHORT, NULL);
                return;
            }
            ethernet = (struct ndpi_ethhdr *)&packet[eth_offset];
            ip_offset = sizeof(struct ndpi_ethhdr) + eth_offset;
            type = ntohs(ethernet->h_proto);
            switch (type)
            {
                case ETH_P_IP: /* IPv4 */
                    if (header->len < sizeof(struct ndpi_ethhdr) + sizeof(struct ndpi_iphdr))
                    {
                        jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
                        jsonize_basic_eventf(reader_thread, IP4_PACKET_TOO_SHORT, NULL);
                        return;
                    }
                    break;
                case ETH_P_IPV6: /* IPV6 */
                    if (header->len < sizeof(struct ndpi_ethhdr) + sizeof(struct ndpi_ipv6hdr))
                    {
                        jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
                        jsonize_basic_eventf(reader_thread, IP6_PACKET_TOO_SHORT, NULL);
                        return;
                    }
                    break;
                case ETH_P_ARP: /* ARP */
                    return;
                default:
                    jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
                    jsonize_basic_eventf(reader_thread, ETHERNET_PACKET_UNKNOWN, "%s%u", "type", type);
                    return;
            }
            break;
        default:
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(
                reader_thread, UNKNOWN_DATALINK_LAYER, "%s%u", "datalink", pcap_datalink(workflow->pcap_handle));
            return;
    }

    if (type == ETH_P_IP)
    {
        ip = (struct ndpi_iphdr *)&packet[ip_offset];
        ip6 = NULL;
    }
    else if (type == ETH_P_IPV6)
    {
        ip = NULL;
        ip6 = (struct ndpi_ipv6hdr *)&packet[ip_offset];
    }
    else
    {
        jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
        jsonize_basic_eventf(reader_thread, UNKNOWN_L3_PROTOCOL, "%s%u", "protocol", type);
        return;
    }
    ip_size = header->len - ip_offset;

    if (type == ETH_P_IP && header->len >= ip_offset)
    {
        if (header->caplen < header->len)
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(reader_thread,
                                 CAPTURE_SIZE_SMALLER_THAN_PACKET_SIZE,
                                 "%s%u %s%u",
                                 "caplen",
                                 header->caplen,
                                 "len",
                                 header->len);
        }
    }

    /* process layer3 e.g. IPv4 / IPv6 */
    if (ip != NULL && ip->version == 4)
    {
        if (ip_size < sizeof(*ip))
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(
                reader_thread, IP4_SIZE_SMALLER_THAN_HEADER, "%s%u %s%zu", "ip_size", ip_size, "expected", sizeof(*ip));
            return;
        }

        flow.l3_type = L3_IP;
        if (ndpi_detection_get_l4(
                (uint8_t *)ip, ip_size, &l4_ptr, &l4_len, &flow.l4_protocol, NDPI_DETECTION_ONLY_IPV4) != 0)
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(
                reader_thread, IP4_L4_PAYLOAD_DETECTION_FAILED, "%s%zu", "l4_data_len", ip_size - sizeof(*ip));
            return;
        }

        flow.ip_tuple.v4.src = ip->saddr;
        flow.ip_tuple.v4.dst = ip->daddr;
        uint32_t min_addr = (flow.ip_tuple.v4.src > flow.ip_tuple.v4.dst ? flow.ip_tuple.v4.dst : flow.ip_tuple.v4.src);
        thread_index = min_addr + ip->protocol;
    }
    else if (ip6 != NULL)
    {
        if (ip_size < sizeof(ip6->ip6_hdr))
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(reader_thread,
                                 IP6_SIZE_SMALLER_THAN_HEADER,
                                 "%s%u %s%zu",
                                 "ip_size",
                                 ip_size,
                                 "expected",
                                 sizeof(ip6->ip6_hdr));
            return;
        }

        flow.l3_type = L3_IP6;
        if (ndpi_detection_get_l4(
                (uint8_t *)ip6, ip_size, &l4_ptr, &l4_len, &flow.l4_protocol, NDPI_DETECTION_ONLY_IPV6) != 0)
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(
                reader_thread, IP6_L4_PAYLOAD_DETECTION_FAILED, "%s%zu", "l4_data_len", ip_size - sizeof(*ip));
            return;
        }

        flow.ip_tuple.v6.src[0] = ip6->ip6_src.u6_addr.u6_addr64[0];
        flow.ip_tuple.v6.src[1] = ip6->ip6_src.u6_addr.u6_addr64[1];
        flow.ip_tuple.v6.dst[0] = ip6->ip6_dst.u6_addr.u6_addr64[0];
        flow.ip_tuple.v6.dst[1] = ip6->ip6_dst.u6_addr.u6_addr64[1];
        uint64_t min_addr[2];
        if (flow.ip_tuple.v6.src[0] > flow.ip_tuple.v6.dst[0] && flow.ip_tuple.v6.src[1] > flow.ip_tuple.v6.dst[1])
        {
            min_addr[0] = flow.ip_tuple.v6.dst[0];
            min_addr[1] = flow.ip_tuple.v6.dst[0];
        }
        else
        {
            min_addr[0] = flow.ip_tuple.v6.src[0];
            min_addr[1] = flow.ip_tuple.v6.src[0];
        }
        thread_index = min_addr[0] + min_addr[1] + ip6->ip6_hdr.ip6_un1_nxt;
    }
    else
    {
        jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
        jsonize_basic_eventf(reader_thread, UNKNOWN_L3_PROTOCOL, "%s%u", "protocol", type);
        return;
    }

    /* process layer4 e.g. TCP / UDP */
    if (flow.l4_protocol == IPPROTO_TCP)
    {
        const struct ndpi_tcphdr * tcp;

        if (header->len < (l4_ptr - packet) + sizeof(struct ndpi_tcphdr))
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(reader_thread,
                                 TCP_PACKET_TOO_SHORT,
                                 "%s%u %s%zu",
                                 "header_len",
                                 header->len,
                                 "expected",
                                 (l4_ptr - packet) + sizeof(struct ndpi_tcphdr));
            return;
        }
        tcp = (struct ndpi_tcphdr *)l4_ptr;
        flow.is_midstream_flow = (tcp->syn == 0 ? 1 : 0);
        flow.flow_fin_ack_seen = (tcp->fin == 1 && tcp->ack == 1 ? 1 : 0);
        flow.flow_ack_seen = tcp->ack;
        flow.src_port = ntohs(tcp->source);
        flow.dst_port = ntohs(tcp->dest);
    }
    else if (flow.l4_protocol == IPPROTO_UDP)
    {
        const struct ndpi_udphdr * udp;

        if (header->len < (l4_ptr - packet) + sizeof(struct ndpi_udphdr))
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(reader_thread,
                                 UDP_PACKET_TOO_SHORT,
                                 "%s%u %s%zu",
                                 "header_len",
                                 header->len,
                                 "expected",
                                 (l4_ptr - packet) + sizeof(struct ndpi_udphdr));
            return;
        }
        udp = (struct ndpi_udphdr *)l4_ptr;
        flow.src_port = ntohs(udp->source);
        flow.dst_port = ntohs(udp->dest);
    }

    /* distribute flows to threads while keeping stability (same flow goes always to same thread) */
    thread_index += (flow.src_port < flow.dst_port ? flow.dst_port : flow.src_port);
    thread_index %= reader_thread_count;
    if (thread_index != reader_thread->array_index)
    {
        return;
    }
    workflow->packets_processed++;
    workflow->total_l4_data_len += l4_len;

    /* calculate flow hash for btree find, search(insert) */
    switch (flow.l3_type)
    {
        case L3_IP:
            if (ndpi_flowv4_flow_hash(flow.l4_protocol,
                                      flow.ip_tuple.v4.src,
                                      flow.ip_tuple.v4.dst,
                                      flow.src_port,
                                      flow.dst_port,
                                      0,
                                      0,
                                      (uint8_t *)&flow.hashval,
                                      sizeof(flow.hashval)) != 0)
            {
                flow.hashval = flow.ip_tuple.v4.src + flow.ip_tuple.v4.dst; // fallback
            }
            break;
        case L3_IP6:
            if (ndpi_flowv6_flow_hash(flow.l4_protocol,
                                      &ip6->ip6_src,
                                      &ip6->ip6_dst,
                                      flow.src_port,
                                      flow.dst_port,
                                      0,
                                      0,
                                      (uint8_t *)&flow.hashval,
                                      sizeof(flow.hashval)) != 0)
            {
                flow.hashval = flow.ip_tuple.v6.src[0] + flow.ip_tuple.v6.src[1];
                flow.hashval += flow.ip_tuple.v6.dst[0] + flow.ip_tuple.v6.dst[1];
            }
            break;
    }
    flow.hashval += flow.l4_protocol + flow.src_port + flow.dst_port;

    hashed_index = flow.hashval % workflow->max_active_flows;
    tree_result = ndpi_tfind(&flow, &workflow->ndpi_flows_active[hashed_index], ndpi_workflow_node_cmp);
    if (tree_result == NULL)
    {
        /* flow not found in btree: switch src <-> dst and try to find it again */
        uint64_t orig_src_ip[2] = {flow.ip_tuple.v6.src[0], flow.ip_tuple.v6.src[1]};
        uint64_t orig_dst_ip[2] = {flow.ip_tuple.v6.dst[0], flow.ip_tuple.v6.dst[1]};
        uint16_t orig_src_port = flow.src_port;
        uint16_t orig_dst_port = flow.dst_port;

        flow.ip_tuple.v6.src[0] = orig_dst_ip[0];
        flow.ip_tuple.v6.src[1] = orig_dst_ip[1];
        flow.ip_tuple.v6.dst[0] = orig_src_ip[0];
        flow.ip_tuple.v6.dst[1] = orig_src_ip[1];
        flow.src_port = orig_dst_port;
        flow.dst_port = orig_src_port;

        tree_result = ndpi_tfind(&flow, &workflow->ndpi_flows_active[hashed_index], ndpi_workflow_node_cmp);
        if (tree_result != NULL)
        {
            direction_changed = 1;
        }

        flow.ip_tuple.v6.src[0] = orig_src_ip[0];
        flow.ip_tuple.v6.src[1] = orig_src_ip[1];
        flow.ip_tuple.v6.dst[0] = orig_dst_ip[0];
        flow.ip_tuple.v6.dst[1] = orig_dst_ip[1];
        flow.src_port = orig_src_port;
        flow.dst_port = orig_dst_port;
    }

    if (tree_result == NULL)
    {
        /* flow still not found, must be new */
        if (workflow->cur_active_flows == workflow->max_active_flows)
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(reader_thread,
                                 MAX_FLOW_TO_TRACK,
                                 "%s%llu %s%llu %s%llu",
                                 "current_active",
                                 workflow->max_active_flows,
                                 "current_idle",
                                 workflow->cur_idle_flows,
                                 "max_active",
                                 workflow->max_active_flows);
            return;
        }

        flow_to_process = (struct nDPId_flow_info *)ndpi_malloc(sizeof(*flow_to_process));
        if (flow_to_process == NULL)
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(
                reader_thread, FLOW_MEMORY_ALLOCATION_FAILED, "%s%zu", "size", sizeof(*flow_to_process));
            return;
        }

        workflow->cur_active_flows++;
        workflow->total_active_flows++;
        memcpy(flow_to_process, &flow, sizeof(*flow_to_process));
#ifdef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_4
        flow_to_process->flow_id = __sync_fetch_and_add(&global_flow_id, 1);
#else
#warning "Compare and Fetch aka __sync_fetch_and_add not available on your platform/compiler, do not trust any flow_id!"
        flow_to_process->flow_id = global_flow_id++;
#endif

        flow_to_process->ndpi_flow = (struct ndpi_flow_struct *)ndpi_flow_malloc(SIZEOF_FLOW_STRUCT);
        if (flow_to_process->ndpi_flow == NULL)
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(reader_thread,
                                 NDPI_FLOW_MEMORY_ALLOCATION_FAILED,
                                 "%s%u %s%zu",
                                 "flow_id",
                                 flow_to_process->flow_id,
                                 "size",
                                 SIZEOF_FLOW_STRUCT);
            return;
        }
        memset(flow_to_process->ndpi_flow, 0, SIZEOF_FLOW_STRUCT);

        flow_to_process->ndpi_src = (struct ndpi_id_struct *)ndpi_calloc(1, SIZEOF_ID_STRUCT);
        if (flow_to_process->ndpi_src == NULL)
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(reader_thread,
                                 NDPI_ID_MEMORY_ALLOCATION_FAILED,
                                 "%s%u %s%zu %s%s",
                                 "flow_id",
                                 flow_to_process->flow_id,
                                 "size",
                                 SIZEOF_ID_STRUCT,
                                 "direction",
                                 "src");
            return;
        }

        flow_to_process->ndpi_dst = (struct ndpi_id_struct *)ndpi_calloc(1, SIZEOF_ID_STRUCT);
        if (flow_to_process->ndpi_dst == NULL)
        {
            jsonize_packet_event(reader_thread, header, packet, NULL, PACKET_EVENT_PAYLOAD);
            jsonize_basic_eventf(reader_thread,
                                 NDPI_ID_MEMORY_ALLOCATION_FAILED,
                                 "%s%u %s%zu %s%s",
                                 "flow_id",
                                 flow_to_process->flow_id,
                                 "size",
                                 SIZEOF_ID_STRUCT,
                                 "direction",
                                 "dst");
            return;
        }
        if (ndpi_tsearch(flow_to_process, &workflow->ndpi_flows_active[hashed_index], ndpi_workflow_node_cmp) == NULL)
        {
            /* Possible Leak, but should not happen as we'd abort earlier. */
            return;
        }

        ndpi_src = flow_to_process->ndpi_src;
        ndpi_dst = flow_to_process->ndpi_dst;

        is_new_flow = 1;
    }
    else
    {
        flow_to_process = *(struct nDPId_flow_info **)tree_result;

        if (direction_changed != 0)
        {
            ndpi_src = flow_to_process->ndpi_dst;
            ndpi_dst = flow_to_process->ndpi_src;
        }
        else
        {
            ndpi_src = flow_to_process->ndpi_src;
            ndpi_dst = flow_to_process->ndpi_dst;
        }
    }

    flow_to_process->packets_processed++;
    flow_to_process->total_l4_data_len += l4_len;
    /* update timestamps, important for timeout handling */
    if (flow_to_process->first_seen == 0)
    {
        flow_to_process->first_seen = time_ms;
    }
    flow_to_process->last_seen = time_ms;
    /* current packet is an TCP-ACK? */
    flow_to_process->flow_ack_seen = flow.flow_ack_seen;

    if (l4_len > flow_to_process->max_l4_data_len)
    {
        flow_to_process->max_l4_data_len = l4_len;
    }
    if (l4_len < flow_to_process->min_l4_data_len)
    {
        flow_to_process->min_l4_data_len = l4_len;
    }

    jsonize_packet_event(reader_thread, header, packet, flow_to_process, PACKET_EVENT_PAYLOAD_FLOW);

    if (is_new_flow != 0)
    {
        flow_to_process->max_l4_data_len = l4_len;
        flow_to_process->min_l4_data_len = l4_len;
        jsonize_flow_event(reader_thread, flow_to_process, FLOW_EVENT_NEW);
    }

    /* TCP-FIN: indicates that at least one side wants to end the connection */
    if (flow.flow_fin_ack_seen != 0 && flow_to_process->flow_fin_ack_seen == 0)
    {
        flow_to_process->flow_fin_ack_seen = 1;
        jsonize_flow_event(reader_thread, flow_to_process, FLOW_EVENT_END);
        return;
    }

    if (flow_to_process->ndpi_flow->num_processed_pkts == 0xFF)
    {
        return;
    }
    else if (flow_to_process->ndpi_flow->num_processed_pkts == 0xFE)
    {
        if (flow_to_process->detection_completed != 0)
        {
            jsonize_flow_event(reader_thread, flow_to_process, FLOW_EVENT_DETECTED);
        }
        else
        {
            /* last chance to guess something, better then nothing */
            uint8_t protocol_was_guessed = 0;
            flow_to_process->guessed_protocol =
                ndpi_detection_giveup(workflow->ndpi_struct, flow_to_process->ndpi_flow, 1, &protocol_was_guessed);
            if (protocol_was_guessed != 0)
            {
                jsonize_flow_event(reader_thread, flow_to_process, FLOW_EVENT_GUESSED);
            }
            else
            {
                jsonize_flow_event(reader_thread, flow_to_process, FLOW_EVENT_NOT_DETECTED);
            }
        }
    }

    flow_to_process->detected_l7_protocol = ndpi_detection_process_packet(workflow->ndpi_struct,
                                                                          flow_to_process->ndpi_flow,
                                                                          ip != NULL ? (uint8_t *)ip : (uint8_t *)ip6,
                                                                          ip_size,
                                                                          time_ms,
                                                                          ndpi_src,
                                                                          ndpi_dst);

    if (ndpi_is_protocol_detected(workflow->ndpi_struct, flow_to_process->detected_l7_protocol) != 0 &&
        flow_to_process->detection_completed == 0)
    {
        if (flow_to_process->detected_l7_protocol.master_protocol != NDPI_PROTOCOL_UNKNOWN ||
            flow_to_process->detected_l7_protocol.app_protocol != NDPI_PROTOCOL_UNKNOWN)
        {
            flow_to_process->detection_completed = 1;
            workflow->detected_flow_protocols++;
            jsonize_flow_event(reader_thread, flow_to_process, FLOW_EVENT_DETECTED);
        }
    }
}

static void run_pcap_loop(struct nDPId_reader_thread const * const reader_thread)
{
    if (reader_thread->workflow != NULL && reader_thread->workflow->pcap_handle != NULL)
    {

        if (pcap_loop(reader_thread->workflow->pcap_handle, -1, &ndpi_process_packet, (uint8_t *)reader_thread) ==
            PCAP_ERROR)
        {

            syslog(LOG_DAEMON | LOG_ERR,
                   "Error while reading pcap file: '%s'\n",
                   pcap_geterr(reader_thread->workflow->pcap_handle));
            reader_thread->workflow->error_or_eof = 1;
        }
    }
}

static void break_pcap_loop(struct nDPId_reader_thread * const reader_thread)
{
    if (reader_thread->workflow != NULL && reader_thread->workflow->pcap_handle != NULL)
    {
        pcap_breakloop(reader_thread->workflow->pcap_handle);
    }
}

static void * processing_thread(void * const ndpi_thread_arg)
{
    struct nDPId_reader_thread * const reader_thread = (struct nDPId_reader_thread *)ndpi_thread_arg;

    reader_thread->json_sockfd = -1;
    reader_thread->json_sock_reconnect = 1;
    if (connect_to_json_socket(reader_thread) != 0)
    {
        syslog(LOG_DAEMON | LOG_ERR,
               "Thread %u: Could not connect to JSON sink, will try again later",
               reader_thread->array_index);
    }
    run_pcap_loop(reader_thread);
    reader_thread->workflow->error_or_eof = 1;
    return NULL;
}

static int processing_threads_error_or_eof(void)
{
    for (int i = 0; i < reader_thread_count; ++i)
    {
        if (reader_threads[i].workflow->error_or_eof == 0)
        {
            return 0;
        }
    }
    return 1;
}

static int start_reader_threads(void)
{
    sigset_t thread_signal_set, old_signal_set;

    sigfillset(&thread_signal_set);
    sigdelset(&thread_signal_set, SIGINT);
    sigdelset(&thread_signal_set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &thread_signal_set, &old_signal_set) != 0)
    {
        syslog(LOG_DAEMON | LOG_ERR, "pthread_sigmask: %s\n", strerror(errno));
        return 1;
    }

    openlog("nDPId", LOG_CONS | (log_to_stderr != 0 ? LOG_PERROR : 0), LOG_DAEMON);

    for (int i = 0; i < reader_thread_count; ++i)
    {
        reader_threads[i].array_index = i;

        if (reader_threads[i].workflow == NULL)
        {
            /* no more threads should be started */
            break;
        }

        if (pthread_create(&reader_threads[i].thread_id, NULL, processing_thread, &reader_threads[i]) != 0)
        {
            syslog(LOG_DAEMON | LOG_ERR, "pthread_create: %s\n", strerror(errno));
            return 1;
        }
    }

    if (pthread_sigmask(SIG_BLOCK, &old_signal_set, NULL) != 0)
    {
        syslog(LOG_DAEMON | LOG_ERR, "pthread_sigmask: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}

static int stop_reader_threads(void)
{
    unsigned long long int total_packets_processed = 0;
    unsigned long long int total_l4_data_len = 0;
    unsigned long long int total_flows_captured = 0;
    unsigned long long int total_flows_idle = 0;
    unsigned long long int total_flows_detected = 0;

    for (int i = 0; i < reader_thread_count; ++i)
    {
        break_pcap_loop(&reader_threads[i]);
    }

    printf("------------------------------------ Stopping reader threads\n");
    for (int i = 0; i < reader_thread_count; ++i)
    {
        if (reader_threads[i].workflow == NULL)
        {
            continue;
        }

        if (pthread_join(reader_threads[i].thread_id, NULL) != 0)
        {
            syslog(LOG_DAEMON | LOG_ERR, "pthread_join: %s\n", strerror(errno));
        }
    }

    printf("------------------------------------ Results\n");
    for (int i = 0; i < reader_thread_count; ++i)
    {
        if (reader_threads[i].workflow == NULL)
        {
            continue;
        }

        total_packets_processed += reader_threads[i].workflow->packets_processed;
        total_l4_data_len += reader_threads[i].workflow->total_l4_data_len;
        total_flows_captured += reader_threads[i].workflow->total_active_flows;
        total_flows_idle += reader_threads[i].workflow->total_idle_flows;
        total_flows_detected += reader_threads[i].workflow->detected_flow_protocols;

        printf(
            "Stopping Thread %d, processed %10llu packets, %12llu bytes, total flows: %8llu, "
            "idle flows: %8llu, detected flows: %8llu\n",
            reader_threads[i].array_index,
            reader_threads[i].workflow->packets_processed,
            reader_threads[i].workflow->total_l4_data_len,
            reader_threads[i].workflow->total_active_flows,
            reader_threads[i].workflow->total_idle_flows,
            reader_threads[i].workflow->detected_flow_protocols);
    }
    /* total packets captured: same value for all threads as packet2thread distribution happens later */
    printf("Total packets captured.: %llu\n", reader_threads[0].workflow->packets_captured);
    printf("Total packets processed: %llu\n", total_packets_processed);
    printf("Total layer4 data size.: %llu\n", total_l4_data_len);
    printf("Total flows captured...: %llu\n", total_flows_captured);
    printf("Total flows timed out..: %llu\n", total_flows_idle);
    printf("Total flows detected...: %llu\n", total_flows_detected);

    return 0;
}

static void free_reader_threads(void)
{
    for (int i = 0; i < reader_thread_count; ++i)
    {
        if (reader_threads[i].workflow == NULL)
        {
            continue;
        }

        free_workflow(&reader_threads[i].workflow);
    }
}

static void sighandler(int signum)
{
    syslog(LOG_DAEMON | LOG_NOTICE, "Received SIGNAL %d\n", signum);

    if (main_thread_shutdown == 0)
    {
        main_thread_shutdown = 1;
        if (stop_reader_threads() != 0)
        {
            syslog(LOG_DAEMON | LOG_ERR, "Failed to stop reader threads!\n");
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        syslog(LOG_DAEMON | LOG_NOTICE, "Reader threads are already shutting down, please be patient.\n");
    }
}

static int parse_options(int argc, char ** argv)
{
    int opt;

    while ((opt = getopt(argc, argv, "hi:lc:")) != -1)
    {
        switch (opt)
        {
            case 'i':
                pcap_file_or_interface = strdup(optarg);
                break;
            case 'l':
                log_to_stderr = 1;
                break;
            case 'c':
                strncpy(json_sockpath, optarg, sizeof(json_sockpath) - 1);
                json_sockpath[sizeof(json_sockpath) - 1] = '\0';
                break;
            default:
                fprintf(stderr, "Usage: %s [-i pcap-file/interface ] [-l] [-c path-to-unix-sock]\n", argv[0]);
                return 1;
        }
    }

    return 0;
}

int main(int argc, char ** argv)
{
    if (argc == 0)
    {
        return 1;
    }

    if (parse_options(argc, argv) != 0)
    {
        return 1;
    }

    printf(
        "----------------------------------\n"
        "nDPI version: %s\n"
        " API version: %u\n"
        "pcap version: %s\n"
        "----------------------------------\n",
        ndpi_revision(),
        ndpi_get_api_version(),
        pcap_lib_version() + strlen("libpcap version "));

    openlog("nDPId", LOG_CONS | LOG_PERROR, LOG_DAEMON);

    if (setup_reader_threads(pcap_file_or_interface) != 0)
    {
        syslog(LOG_DAEMON | LOG_ERR, "%s: setup_reader_threads failed\n", argv[0]);
        return 1;
    }

    if (start_reader_threads() != 0)
    {
        syslog(LOG_DAEMON | LOG_ERR, "%s: start_reader_threads\n", argv[0]);
        return 1;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    while (main_thread_shutdown == 0 && processing_threads_error_or_eof() == 0)
    {
        sleep(1);
    }

    if (main_thread_shutdown == 0 && stop_reader_threads() != 0)
    {
        syslog(LOG_DAEMON | LOG_ERR, "%s: stop_reader_threads\n", argv[0]);
        return 1;
    }
    free_reader_threads();

    closelog();

    return 0;
}