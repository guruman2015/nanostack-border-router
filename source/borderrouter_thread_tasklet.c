/*
 * Copyright (c) 2016 ARM Limited. All rights reserved.
 */

#ifdef MBED_CONF_APP_THREAD_BR

#include <string.h>
#include <stdlib.h>
#include <mbed_assert.h>
#include "ns_types.h"
#include "eventOS_event.h"
#include "eventOS_event_timer.h"
#include "eventOS_scheduler.h"
#include "platform/arm_hal_timer.h"
#include "nanostack-border-router/borderrouter_tasklet.h"
#include "nanostack-border-router/borderrouter_helpers.h"
#include "rf_wrapper.h"
#include "nwk_stats_api.h"
#include "net_interface.h"
#include "ip6string.h"
#include "ethernet_mac_api.h"
#include "mac_api.h"
#include "sw_mac.h"
#include "mbed_interface.h"
#include "common_functions.h"
#include "thread_management_if.h"
#include "thread_interface_status.h"
#include "nanostack-border-router/mbed_config.h"
#include "randLIB.h"

#include "ns_trace.h"
#define TRACE_GROUP "brro"

#define NR_BACKHAUL_INTERFACE_PHY_DRIVER_READY 2
#define NR_BACKHAUL_INTERFACE_PHY_DOWN  3
#define MESH_LINK_TIMEOUT 100
#define MESH_METRIC 1000
#define THREAD_MAX_CHILD_COUNT 32

static mac_api_t *api;
static eth_mac_api_t *eth_mac_api;

typedef enum interface_bootstrap_state {
    INTERFACE_IDLE_PHY_NOT_READY,
    INTERFACE_IDLE_STATE,
    INTERFACE_BOOTSTRAP_ACTIVE,
    INTERFACE_CONNECTED
} interface_bootstrap_state_e;

typedef struct {
    int8_t prefix_len;
    uint8_t prefix[16];
    uint8_t next_hop[16];
} route_info_t;

/* Border router channel list */
static channel_list_s channel_list;

/* Backhaul prefix */
static uint8_t backhaul_prefix[16] = {0};

/* Backhaul default route information */
static route_info_t backhaul_route;
static int8_t br_tasklet_id = -1;
static int8_t backhaul_if_id = -1;

/* Network statistics */
static nwk_stats_t nwk_stats;

/* Function forward declarations */

static void thread_link_configuration_get(link_configuration_s *link_configuration);
static void network_interface_event_handler(arm_event_s *event);

static void meshnetwork_up();
static void eth_network_data_init(void);
static interface_bootstrap_state_e net_6lowpan_state = INTERFACE_IDLE_PHY_NOT_READY;
static interface_bootstrap_state_e net_backhaul_state = INTERFACE_IDLE_PHY_NOT_READY;
static net_ipv6_mode_e backhaul_bootstrap_mode = NET_IPV6_BOOTSTRAP_STATIC;
static void borderrouter_tasklet(arm_event_s *event);

typedef enum {
    STATE_UNKNOWN,
    STATE_DISCONNECTED,
    STATE_LINK_READY,
    STATE_BOOTSTRAP,
    STATE_CONNECTED,
    STATE_MAX_VALUE
} connection_state_e;

typedef struct {
	connection_state_e  state;    
    int8_t              driver_id;
    uint8_t             mac48[6];
    uint8_t             global_address[16];        
} if_ethernet_t;


typedef struct {
    connection_state_e state;
    int8_t          rf_driver_id;
    int8_t          net_rf_id;        
} if_mesh_t;


typedef struct {
    if_mesh_t       mesh;
    if_ethernet_t   ethernet;        
    int8_t tasklet_id;
} network_configuration_t;

/** Net start operation mode and feature variables*/
network_configuration_t network;

static void eth_network_data_init()
{
	memset(&backhaul_prefix[8], 0, 8);  

    /* Bootstrap mode for the backhaul interface */
#if MBED_CONF_APP_BACKHAUL_DYNAMIC_BOOTSTRAP == 1
        backhaul_bootstrap_mode = NET_IPV6_BOOTSTRAP_AUTONOMOUS;
        tr_info("NET_IPV6_BOOTSTRAP_AUTONOMOUS");
    
#else
		tr_info("NET_IPV6_BOOTSTRAP_STATIC");
        backhaul_bootstrap_mode = NET_IPV6_BOOTSTRAP_STATIC;								
		// done like this so that prefix can be left out in the dynamic case.
		const char* param = MBED_CONF_APP_BACKHAUL_PREFIX; 		
		stoip6(param, strlen(param), backhaul_prefix);
		tr_info("backhaul_prefix: %s", print_ipv6(backhaul_prefix));		
		
		/* Backhaul route configuration*/
		memset(&backhaul_route, 0, sizeof(backhaul_route));
		#ifdef MBED_CONF_APP_BACKHAUL_NEXT_HOP
		param = MBED_CONF_APP_BACKHAUL_NEXT_HOP; 		
		stoip6(param, strlen(param), backhaul_route.next_hop);    
		tr_info("next hop: %s", print_ipv6(backhaul_route.next_hop));
		#endif
		param = MBED_CONF_APP_BACKHAUL_DEFAULT_ROUTE;		
		char *prefix, route_buf[255] = {0};
		/* copy the config value to a non-const buffer */
		strncpy(route_buf, param, sizeof(route_buf) - 1);
		prefix = strtok(route_buf, "/");
		backhaul_route.prefix_len = atoi(strtok(NULL, "/"));
		stoip6(prefix, strlen(prefix), backhaul_route.prefix);
		tr_info("backhaul route prefix: %s", print_ipv6(backhaul_route.prefix));	
#endif
	
	network.ethernet.driver_id = -1;
    network.ethernet.state = STATE_DISCONNECTED;            
	
	char rf_mac[6] = {0};
    mbed_mac_address(rf_mac);
    memcpy(network.ethernet.mac48, rf_mac, sizeof(rf_mac));	
	
}

static int thread_interface_up()
{
    int32_t val;    
    device_configuration_s device_config;
    link_configuration_s link_setup;

	tr_info("thread_interface_up");
    memset(&device_config, 0, sizeof(device_config));
	
	const char* param = MBED_CONF_APP_PSKD;
	uint16_t len = strlen(param);
	MBED_ASSERT(len>5 && len <33);	
	
    device_config.PSKd_ptr = malloc(len+1);
    device_config.PSKd_len = len;
	memset(device_config.PSKd_ptr, 0, len + 1);
	memcpy(device_config.PSKd_ptr, param, len);    	
    
    thread_link_configuration_get(&link_setup);
    
    val = thread_management_node_init(network.mesh.net_rf_id, &channel_list, &device_config, &link_setup);
	
    if (val) {
        tr_error("Thread init error with code: %is\r\n", (int)val);
		return val;
    }

    // Additional thread configurations    	
    thread_management_set_link_timeout(network.mesh.net_rf_id, MESH_LINK_TIMEOUT);   
    thread_management_max_child_count(network.mesh.net_rf_id, THREAD_MAX_CHILD_COUNT);        

    val = arm_nwk_interface_up(network.mesh.net_rf_id);
    if (val != 0) {
        tr_error("\rmesh0 up Fail with code: %i\r\n", (int)val);
        network.mesh.state = STATE_UNKNOWN;        
        return -1;
    } else {
        tr_info("\rmesh0 bootstrap ongoing..\r\n");
        network.mesh.state = STATE_BOOTSTRAP;        
    }

    return 0;
}

static void thread_link_configuration_get(link_configuration_s *link_configuration)
{   	
	memset(link_configuration, 0, sizeof(link_configuration_s));    	
		
	MBED_ASSERT(strlen(MBED_CONF_APP_NETWORK_NAME)>0 && strlen(MBED_CONF_APP_NETWORK_NAME) < 17);	
	const uint8_t master_key[] = MBED_CONF_APP_THREAD_MASTER_KEY; 
	MBED_ASSERT(sizeof(master_key) == 16);
	const uint8_t pskc[] = MBED_CONF_APP_PSKC;	
	MBED_ASSERT(sizeof(pskc)==16);
	
	const uint8_t extented_panid[] = MBED_CONF_APP_EXTENDED_PAN_ID;
	MBED_ASSERT(sizeof(extented_panid) == 8);
	const uint8_t mesh_local_ula_prefix[] = MBED_CONF_APP_MESH_LOCAL_ULA_PREFIX;	
	MBED_ASSERT(sizeof(mesh_local_ula_prefix) == 8);
	
    memcpy(link_configuration->name, MBED_CONF_APP_NETWORK_NAME, 16);
	
    memcpy(link_configuration->extented_pan_id, extented_panid, sizeof(extented_panid));
    memcpy(link_configuration->PSKc, pskc, sizeof(pskc));
	memcpy(link_configuration->master_key, master_key, 16);
    link_configuration->panId = MBED_CONF_APP_PAN_ID;
    link_configuration->rfChannel = MBED_CONF_APP_RF_CHANNEL;
	link_configuration->channel_page = MBED_CONF_APP_RF_CHANNEL_PAGE;
	
	//uint8_t channel_mask[8];
	uint32_t channel_mask = MBED_CONF_APP_RF_CHANNEL_MASK;
	common_write_32_bit(link_configuration->channel_mask, &channel_mask);	
    link_configuration->securityPolicy = SECURITY_POLICY_ALL_SECURITY;		
	
    memcpy(link_configuration->mesh_local_ula_prefix, mesh_local_ula_prefix, sizeof(mesh_local_ula_prefix));    
    link_configuration->key_rotation = 3600;
    link_configuration->key_sequence = 0;
}

static void network_interface_event_handler(arm_event_s *event)
{
    bool connectStatus = false;
    arm_nwk_interface_status_type_e status = (arm_nwk_interface_status_type_e)event->event_data;
    switch (status) {
        case (ARM_NWK_BOOTSTRAP_READY): { // Interface configured Bootstrap is ready

            connectStatus = true;
            tr_info("BR interface_id %d.", thread_interface_status_border_router_interface_id_get());
            if (-1 != thread_interface_status_border_router_interface_id_get()) {
                if (0 == arm_net_address_get(thread_interface_status_border_router_interface_id_get(), ADDR_IPV6_GP, network.ethernet.global_address)) {
                    network.ethernet.state = STATE_CONNECTED;                    
                    tr_info("Ethernet (eth0) bootstrap ready. IP: %s", print_ipv6(network.ethernet.global_address));

					// metric set to high priority
                    if (0 != arm_net_interface_set_metric(thread_interface_status_border_router_interface_id_get(), 0)) {
                        tr_warn("Failed to set metric for eth0.");
                    }

                    if (backhaul_bootstrap_mode==NET_IPV6_BOOTSTRAP_STATIC) {
                        uint8_t *next_hop_ptr;       						
						
						if (memcmp(backhaul_route.next_hop, (const uint8_t[16]) {0}, 16) == 0) {
							next_hop_ptr = NULL;
						}
						else {
							next_hop_ptr = backhaul_route.next_hop;
						}
                        tr_debug("Default route prefix: %s/%d", print_ipv6(backhaul_route.prefix),
                                 backhaul_route.prefix_len);
                        tr_debug("Default route next hop: %s", print_ipv6(backhaul_route.next_hop));
                        arm_net_route_add(backhaul_route.prefix,
                                          backhaul_route.prefix_len,
                                          next_hop_ptr, 0xffffffff, 128,
                                          thread_interface_status_border_router_interface_id_get());
                    }

                    thread_interface_status_prefix_add(network.ethernet.global_address, 64);

                } else {
                    tr_warn("arm_net_address_get fail");
                }
            }
            break;
        }
        case (ARM_NWK_RPL_INSTANCE_FLOODING_READY): // RPL instance have been flooded
            tr_info("\rRPL instance have been flooded\r\n");
            break;
        case (ARM_NWK_SET_DOWN_COMPLETE): // Interface DOWN command successfully
            break;
        case (ARM_NWK_NWK_SCAN_FAIL):   // Interface have not detect any valid network
            tr_warning("\rmesh0 haven't detect any valid nw\r\n");
            break;
        case (ARM_NWK_IP_ADDRESS_ALLOCATION_FAIL): // IP address allocation fail(ND, DHCPv4 or DHCPv6)
            tr_error("\rNO GP address detected");
            break;
        case (ARM_NWK_DUPLICATE_ADDRESS_DETECTED): // User specific GP16 was not valid
            tr_error("\rEthernet IPv6 Duplicate addr detected!\r\n");            
            break;
        case (ARM_NWK_AUHTENTICATION_START_FAIL): // No valid Authentication server detected behind access point ;
            tr_error("\rNo valid ath server detected behind AP\r\n");
            break;
        case (ARM_NWK_AUHTENTICATION_FAIL): // Network authentication fail by Handshake
            tr_error("\rNetwork authentication fail");
            break;
        case (ARM_NWK_NWK_CONNECTION_DOWN): // No connection between Access point or Default Router
            tr_warning("\rPrefix timeout\r\n");
            break;
        case (ARM_NWK_NWK_PARENT_POLL_FAIL): // Sleepy host poll fail 3 time
            tr_warning("\rParent poll fail\r\n");
            break;
        case (ARM_NWK_PHY_CONNECTION_DOWN): // Interface PHY cable off or serial port interface not respond anymore
            tr_error("\reth0 down\r\n");
            break;
        default:
            tr_warning("\rUnkown nw if event (type: %02x, id: %02x, data: %02x)\r\n", event->event_type, event->event_id, (unsigned int)event->event_data);
            break;
    }
    //Update Interface status
    if (connectStatus) {        
    } else {        
        network.ethernet.state = STATE_LINK_READY;
    }  
    thread_interface_status_ethernet_connection(connectStatus);
}

void thread_interface_event_handler(arm_event_s *event)
{
    bool connectStatus = false;
    arm_nwk_interface_status_type_e status = (arm_nwk_interface_status_type_e)event->event_data;
    switch (status) {
        case (ARM_NWK_BOOTSTRAP_READY): { // Interface configured Bootstrap is ready
            connectStatus = true;
            tr_info("\rBootstrap ready\r\n");

            if (arm_net_interface_set_metric(network.mesh.net_rf_id, MESH_METRIC) != 0) {
                tr_warn("Failed to set metric for mesh0.");
            }
            break;
        }        
        case (ARM_NWK_SET_DOWN_COMPLETE):
			tr_info("\rThread interface down\r\n");
            break;        
        default:
            tr_warning("\rUnkown nw if event (type: %02x, id: %02x, data: %02x)\r\n", event->event_type, event->event_id, (unsigned int)event->event_data);
            break;
    }
    if (connectStatus) {
        network.mesh.state = STATE_CONNECTED;
    } else {
        network.mesh.state = STATE_LINK_READY;
    }
	
    thread_interface_status_thread_connection(connectStatus);
}

static void meshnetwork_up() {
        tr_info("mesh0 up");

        if (network.mesh.state == STATE_CONNECTED || network.mesh.state == STATE_BOOTSTRAP) {
            tr_info("mesh0 already up\r\n");
        }        

        if (network.mesh.rf_driver_id != -1) {
            thread_interface_status_thread_driver_id_set(network.mesh.rf_driver_id);
            // Create 6Lowpan Interface
            tr_debug("Create Mesh Interface");            
                
			network.mesh.net_rf_id = arm_nwk_interface_lowpan_init(api, "ThreadInterface");
			tr_info("network.mesh.net_rf_id: %d", network.mesh.net_rf_id);                    
			thread_interface_status_threadinterface_id_set(network.mesh.net_rf_id);                
			
			arm_nwk_interface_configure_6lowpan_bootstrap_set(
				network.mesh.net_rf_id,
				NET_6LOWPAN_ROUTER,
				NET_6LOWPAN_THREAD);       

            if (network.mesh.net_rf_id != -1) {                
				int err = thread_interface_up();
				if(err) {
					 tr_error("thread_interface_up() failed: %d", err);
				}
            }
			else {
				tr_error("arm_nwk_interface_lowpan_init() failed");
			}
        }
    }

void thread_rf_init() {           
    
    mac_description_storage_size_t storage_sizes;
    storage_sizes.key_lookup_size = 1;
    storage_sizes.key_usage_size = 1;
    storage_sizes.device_decription_table_size = 32;
    storage_sizes.key_description_table_size = 6;

	network.mesh.rf_driver_id = rf_device_register();
    randLIB_seed_random();
	eth_network_data_init();

    network.mesh.net_rf_id = -1;     

    if (!api) {
        api = ns_sw_mac_create(network.mesh.rf_driver_id, &storage_sizes);
    }
}

void border_router_start(void)
{
    platform_timer_enable();
    eventOS_scheduler_init();    
    net_init_core();
    protocol_stats_start(&nwk_stats);

    eventOS_event_handler_create(
        &borderrouter_tasklet,
        ARM_LIB_TASKLET_INIT_EVENT);
}


static void borderrouter_backhaul_phy_status_cb(uint8_t link_up, int8_t driver_id)
{
    arm_event_s event;
    event.sender = br_tasklet_id;
    event.receiver = br_tasklet_id;

    if (link_up) {
        event.event_id = NR_BACKHAUL_INTERFACE_PHY_DRIVER_READY;
    } else {
        event.event_id = NR_BACKHAUL_INTERFACE_PHY_DOWN;
    }

    tr_debug("Backhaul driver ID: %d", driver_id);

    event.priority = ARM_LIB_MED_PRIORITY_EVENT;
    event.event_type = APPLICATION_EVENT;
    event.event_data = driver_id;
    event.data_ptr = NULL;
    eventOS_event_send(&event);
}

static int backhaul_interface_up(int8_t driver_id)
{
    int retval = -1;
    tr_debug("backhaul_interface_up: %i\n", driver_id);
    if (backhaul_if_id != -1) {
        tr_debug("Border RouterInterface already at active state\n");
    } else {

        thread_interface_status_borderrouter_driver_id_set(driver_id);        

        if (!eth_mac_api) {
            eth_mac_api = ethernet_mac_create(driver_id);
        }

        backhaul_if_id = arm_nwk_interface_ethernet_init(eth_mac_api, "bh0");

        if (backhaul_if_id >= 0) {
            tr_debug("Backhaul interface ID: %d", backhaul_if_id);            
            thread_interface_status_borderrouter_interface_id_set(backhaul_if_id);        
            arm_nwk_interface_configure_ipv6_bootstrap_set(
                    backhaul_if_id, backhaul_bootstrap_mode, backhaul_prefix);
            arm_nwk_interface_up(backhaul_if_id);
            retval = 0;
        }
    }
    return retval;
}

static int backhaul_interface_down(void)
{
    int retval = -1;
    if (backhaul_if_id != -1) {
        arm_nwk_interface_down(backhaul_if_id);        
        thread_interface_status_borderrouter_interface_id_set(-1);
        thread_interface_status_ethernet_connection(false);
        backhaul_if_id = -1;
        retval = 0;
    }
    return retval;
}

/**
  * \brief Border Router Main Tasklet
  *
  *  Tasklet Handle next items:
  *
  *  - EV_INIT event: Set Certificate Chain, RF Interface Boot UP, multicast Init
  *  - SYSTEM_TIMER event: For RF interface Handshake purpose
  *
  */
static void borderrouter_tasklet(arm_event_s *event)
{
    arm_library_event_type_e event_type;
    event_type = (arm_library_event_type_e)event->event_type;

    switch (event_type) {
        case ARM_LIB_NWK_INTERFACE_EVENT:

            if (event->event_id == thread_interface_status_border_router_interface_id_get()) {
               network_interface_event_handler(event);
            } else {
               thread_interface_event_handler(event);
            }            

            break;
        // comes from the backhaul_driver_init.
        case APPLICATION_EVENT:
            if (event->event_id == NR_BACKHAUL_INTERFACE_PHY_DRIVER_READY) {
                int8_t net_backhaul_id = (int8_t) event->event_data;
                if (net_backhaul_state == INTERFACE_IDLE_PHY_NOT_READY) {
                    net_backhaul_state = INTERFACE_IDLE_STATE;
                }

                if (backhaul_interface_up(net_backhaul_id) != 0) {
                    tr_debug("Backhaul bootstrap start failed");
                } else {
                    tr_debug("Backhaul bootstrap started");
                    net_backhaul_state = INTERFACE_BOOTSTRAP_ACTIVE;
                }
            } else if (event->event_id == NR_BACKHAUL_INTERFACE_PHY_DOWN) {
                if (backhaul_interface_down() != 0) {
                    // may happend when booting first time.
                    tr_warning("Backhaul interface down failed");
                } else {
                    tr_debug("Backhaul interface is down");
                    backhaul_if_id = -1;
                    net_backhaul_state = INTERFACE_IDLE_STATE;
                }
            }
            break;

        case ARM_LIB_TASKLET_INIT_EVENT:
            br_tasklet_id = event->receiver;            
            backhaul_driver_init(borderrouter_backhaul_phy_status_cb);
            thread_interface_status_init();
            thread_rf_init();
            meshnetwork_up();
            net_6lowpan_state = INTERFACE_IDLE_STATE;
            eventOS_event_timer_request(9, ARM_LIB_SYSTEM_TIMER_EVENT, br_tasklet_id, 20000);
            break;

        case ARM_LIB_SYSTEM_TIMER_EVENT:
            eventOS_event_timer_cancel(event->event_id, event->receiver);

            if (event->event_id == 9) {
#ifdef MBED_CONF_APP_DEBUG_TRACE
#if MBED_CONF_APP_DEBUG_TRACE == 1
                arm_print_routing_table();
                arm_print_neigh_cache();
#endif
#endif
                eventOS_event_timer_request(9, ARM_LIB_SYSTEM_TIMER_EVENT, br_tasklet_id, 20000);
            }
            break;

        default:
            break;
    }
}
#endif
