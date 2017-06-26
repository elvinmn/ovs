/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2014 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef BOND_H
#define BOND_H 1

#include <stdbool.h>
#include <stdint.h>
#include "mac-learning.h"
#include "ofproto-provider.h"
#include "packets.h"

struct flow;
struct netdev;
struct ofpbuf;
struct ofproto_dpif;
enum lacp_status;

/* How flows are balanced among bond slaves. */
enum bond_mode {
    BM_TCP, /* Transport Layer Load Balance. */
    BM_SLB, /* Source Load Balance. */
    BM_AB   /* Active Backup. */
};

bool bond_mode_from_string(enum bond_mode *, const char *);
const char *bond_mode_to_string(enum bond_mode);

/* Configuration for a bond as a whole. */
struct bond_settings {
    char *name;                 /* Bond's name, for log messages. */
    uint32_t basis;             /* Flow hashing basis. */

    /* Balancing configuration. */
    enum bond_mode balance;
    int rebalance_interval;     /* Milliseconds between rebalances.
                                   Zero to disable rebalancing. */

    /* Link status detection. */
    int up_delay;               /* ms before enabling an up slave. */
    int down_delay;             /* ms before disabling a down slave. */

    bool lacp_fallback_ab_cfg;  /* Fallback to active-backup on LACP failure. */
    bool lacp_fallback_id_cfg;

    struct eth_addr active_slave_mac;
                                /* The MAC address of the interface
                                   that was active during the last
                                   ovs run. */
};

/* Program startup. */
void bond_init(void);

/* Basics. */
struct bond *bond_create(const struct bond_settings *,
                         struct ofproto_dpif *ofproto);
void bond_unref(struct bond *);
struct bond *bond_ref(const struct bond *);

bool bond_reconfigure(struct bond *, const struct bond_settings *);
void bond_slave_register(struct bond *, void *slave_, ofp_port_t ofport, struct netdev *);
void bond_slave_set_netdev(struct bond *, void *slave_, struct netdev *);
void bond_slave_unregister(struct bond *, const void *slave);

bool bond_run(struct bond *, enum lacp_status);
void bond_wait(struct bond *);

void bond_slave_set_may_enable(struct bond *, void *slave_, bool may_enable);

bool bond_individual(const struct bond *);

/* Special MAC learning support for SLB bonding. */
bool bond_should_send_learning_packets(struct bond *);
struct dp_packet *bond_compose_learning_packet(struct bond *,
                                               const struct eth_addr eth_src,
                                               uint16_t vlan, void **port_aux);
bool bond_get_changed_active_slave(const char *name, struct eth_addr *mac,
                                   bool force);

/* Packet processing. */
enum bond_verdict {
    BV_ACCEPT,                  /* Accept this packet. */
    BV_DROP,                    /* Drop this packet. */
    BV_DROP_IF_MOVED            /* Drop if we've learned a different port. */
};
enum bond_verdict bond_check_admissibility(struct bond *, const void *slave_,
                                           const struct eth_addr dst);
void *bond_choose_output_slave(struct bond *, const struct flow *,
                               struct flow_wildcards *, uint16_t vlan);

/* Rebalancing. */
void bond_account(struct bond *, const struct flow *, uint16_t vlan,
                  uint64_t n_bytes);
void bond_rebalance(struct bond *);

/* Recirculation
 *
 * Only balance_tcp mode uses recirculation.
 *
 * When recirculation is used, each bond port is assigned with a unique
 * recirc_id. The output action to the bond port will be replaced by
 * a Hash action, followed by a RECIRC action.
 *
 *   ... actions= ... HASH(hash(L4)), RECIRC(recirc_id) ....
 *
 * On handling first output packet, 256 post recirculation flows are installed:
 *
 *  recirc_id=<bond_recirc_id>, dp_hash=<[0..255]>/0xff, actions: output<slave>
 *
 * Bond module pulls stats from those post recirculation rules. If rebalancing
 * is needed, those rules are updated with new output actions.
*/
void bond_update_post_recirc_rules(struct bond *, const bool force);
bool bond_may_recirc(const struct bond *, uint32_t *recirc_id,
                     uint32_t *hash_bias);


/* A bond slave, that is, one of the links comprising a bond. */
struct bond_slave {
    struct hmap_node hmap_node; /* In struct bond's slaves hmap. */
    struct ovs_list list_node;  /* In struct bond's enabled_slaves list. */
    struct bond *bond;          /* The bond that contains this slave. */
    void *aux;                  /* Client-provided handle for this slave. */

    struct netdev *netdev;      /* Network device, owned by the client. */
    uint64_t change_seq;        /* Tracks changes in 'netdev'. */
    ofp_port_t  ofp_port;       /* OpenFlow port number. */
    char *name;                 /* Name (a copy of netdev_get_name(netdev)). */

    /* Link status. */
    long long delay_expires;    /* Time after which 'enabled' may change. */
    bool enabled;               /* May be chosen for flows? */
    bool may_enable;            /* Client considers this slave bondable. */

    /* Rebalancing info.  Used only by bond_rebalance(). */
    struct ovs_list bal_node;   /* In bond_rebalance()'s 'bals' list. */
    struct ovs_list entries;    /* 'struct bond_entry's assigned here. */
    uint64_t tx_bytes;          /* Sum across 'tx_bytes' of entries. */
};

/* A bond, that is, a set of network devices grouped to improve performance or
 * robustness.  */
struct bond {
    struct hmap_node hmap_node; /* In 'all_bonds' hmap. */
    char *name;                 /* Name provided by client. */
    struct ofproto_dpif *ofproto; /* The bridge this bond belongs to. */

    /* Slaves. */
    struct hmap slaves;

    /* Enabled slaves.
     *
     * Any reader or writer of 'enabled_slaves' must hold 'mutex'.
     * (To prevent the bond_slave from disappearing they must also hold
     * 'rwlock'.) */
    struct ovs_mutex mutex OVS_ACQ_AFTER(rwlock);
    struct ovs_list enabled_slaves OVS_GUARDED; /* Contains struct bond_slaves. */

    /* Bonding info. */
    enum bond_mode balance;     /* Balancing mode, one of BM_*. */
    struct bond_slave *active_slave;
    int updelay, downdelay;     /* Delay before slave goes up/down, in ms. */
    enum lacp_status lacp_status; /* Status of LACP negotiations. */
    bool bond_revalidate;       /* True if flows need revalidation. */
    uint32_t basis;             /* Basis for flow hash function. */

    /* SLB specific bonding info. */
    struct bond_entry *hash;     /* An array of BOND_BUCKETS elements. */
    int rebalance_interval;      /* Interval between rebalances, in ms. */
    long long int next_rebalance; /* Next rebalancing time. */
    bool send_learning_packets;
    uint32_t recirc_id;          /* Non zero if recirculation can be used.*/
    struct hmap pr_rule_ops;     /* Helps to maintain post recirculation rules.*/

    /* Store active slave to OVSDB. */
    bool active_slave_changed; /* Set to true whenever the bond changes
                                   active slave. It will be reset to false
                                   after it is stored into OVSDB */

    /* Interface name may not be persistent across an OS reboot, use
     * MAC address for identifing the active slave */
    struct eth_addr active_slave_mac;
                               /* The MAC address of the active interface. */
    /* Legacy compatibility. */
    bool lacp_fallback_ab; /* Fallback to active-backup on LACP failure. */
    bool lacp_fallback_id; /* Fallback to active-backup on LACP failure. */

    struct ovs_refcount ref_cnt;

    struct mac_learning *ml;
};

void bond_learn_mac(struct bond *, const void *slave_, const struct eth_addr eth_src, uint16_t vlan, bool is_grat_arp);
#endif /* bond.h */
