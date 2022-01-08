
""" Please make sure you read the README COMPLETELY BEFORE reading anything below.
    It is very critical that you read coding guidelines in Section E in README file.
"""

from xnu import *
from utils import *
from string import *
from net import *

import xnudefines

def IterateProcChannels(proc):
    """ Iterate through all channels in the given process

        params:
            proc - the proc object
        returns: nothing, this is meant to be used as a generator function
            kc - yields each kern_channel in the process
    """

    proc_filedesc = proc.p_fd
    proc_lastfile = unsigned(proc_filedesc.fd_lastfile)
    proc_ofiles = proc_filedesc.fd_ofiles

    count = 0
    while count <= proc_lastfile:
        if unsigned(proc_ofiles[count]) != 0:
            proc_fd_fglob = proc_ofiles[count].fp_glob
            if (unsigned(proc_fd_fglob.fg_ops.fo_type) == 10):
                yield Cast(proc_fd_fglob.fg_data, 'kern_channel *')
        count += 1

def IterateKernChannelRings(kc, kind):
    """ Iterate through all rings on a given channel
    """

    NR_RX = 0
    NR_TX = 1
    NR_A  = 2
    NR_F  = 3

    if kind == NR_RX:
        rings = kc.ch_na.na_rx_rings
    elif kind == NR_TX :
        rings = kc.ch_na.na_tx_rings
    elif kind == NR_A :
        rings = kc.ch_na.na_alloc_rings
    else :
        rings = kc.ch_na.na_free_rings

    # note that ch_last is actually one greater than the last
    # as per the comment in ch_connect
    for i in xrange(kc.ch_first[kind], kc.ch_last[kind]):
        yield addressof(rings[i])

# Note this is broken if you have type summaries enabled
# because we are summarizing the pointer to the structure
# and not the structure itself.  Unfortunately, that's
# the pattern used elsewhere.
# Trying to actually use the type summary will blow up
# because it has a linked list pointer to itself
#
@lldb_type_summary(['kern_channel_t', 'kern_channel *'])
@header('{:<20s} {:<36s}'.format('kern_channel', 'uuid'))
def GetKernChannelSummary(kc):
    """ Summarizes a kern_channel and related information

        returns: str - summary of kern_channel
    """

    format_string = '{o: <#020x} {u: <36s}'
    return format_string.format(
        o=kc,
        u=GetUUIDSummary(kc.ch_info.cinfo_ch_id))

@lldb_type_summary(['__kern_channel_ring *'])
@header('{:<20s} {:<65s} {:>10s} | {:<5s} {:<5s} | {:<5s} {:<5s} | {:<5s} {:<5s}'.format(
        'kernchannelring', 'name', 'flags', 'kh', 'kt', 'rh', 'rt', 'h', 't'))
def GetKernChannelRingSummary(kring):
    """ Summarizes a __kern_channel_ring and related information

        returns: str - summary of kern_channel_ring
    """

    format_string = '{o: <#020x} "{name: <63s}" {flags: >#010x} | {kh: <5d} {kt: <5d} | {rh: <5d} {rt: <5d} | {h: <5d} {t: <5d}'
    return format_string.format(
        o=kring,
        name=kring.ckr_name,
        flags=kring.ckr_flags,
        kh=kring.ckr_khead,
        kt=kring.ckr_ktail,
        rh=kring.ckr_rhead,
        rt=kring.ckr_rtail,
        h=kring.ckr_ring.ring_head,
        t=kring.ckr_ring.ring_tail)

@lldb_command('showprocchannels')
def ShowProcChannels(cmd_args=None):
    """ Show the skywalk channels for a given process.

        usage: showprocchannels <proc_t>
    """

    if not cmd_args:
        raise ArgumentError('missing struct proc * argument')

    proc = kern.GetValueFromAddress(cmd_args[0], 'proc_t')

    print GetKernChannelSummary.header
    for kc in IterateProcChannels(proc):
        print GetKernChannelSummary(kc)

@lldb_command('showchannelrings')
def ShowChannelRings(cmd_args=None):
    """ Show the skywalk rings for a given channel.

        usage: showchannelrings <struct kern_channel *>
    """

    if not cmd_args:
        raise ArgumentError('missing struct kern_channel * argument')

    kc = kern.GetValueFromAddress(cmd_args[0], 'kern_channel *')

    print "RX rings:"
    print GetKernChannelRingSummary.header
    for ring in IterateKernChannelRings(kc, 0) :
        print GetKernChannelRingSummary(ring)

    print "TX rings:"
    print GetKernChannelRingSummary.header
    for ring in IterateKernChannelRings(kc, 1) :
        print GetKernChannelRingSummary(ring)

    print "ALLOC rings:"
    print GetKernChannelRingSummary.header
    for ring in IterateKernChannelRings(kc, 2) :
        print GetKernChannelRingSummary(ring)

    print "FREE rings:"
    print GetKernChannelRingSummary.header
    for ring in IterateKernChannelRings(kc, 3) :
        print GetKernChannelRingSummary(ring)

def SkmemCacheModeAsString(mode) :
    out_string = ""
    SKM_MODE_NOCACHE = 0x1
    SKM_MODE_AUDIT   = 0x2

    if (mode & SKM_MODE_NOCACHE) :
        out_string += "n"
    else :
        out_string += "-"
    if (mode & SKM_MODE_AUDIT) :
        out_string += "a"
    else :
        out_string += "-"

    return out_string

@lldb_command('showskmemcache')
def ShowSkmemCache(cmd_args=None) :
    """ Show the global list of skmem caches
    """

    format_string = "{:<4s}  {:<18s} {:<4s} {:<4s} {:<4s} {:<4s} {:<4s} {:<4s} {:<4s} {:<4s} {:<4s} {:<s}"
    print format_string.format("", "ADDR", "BUFI", "BUFM", "RESC", "SLCR", "SLDE", "SLAL", "SLFR", "DECO", "MODE", "NAME")

    i = 1
    skmhead = kern.globals.skmem_cache_head

    for skm in IterateTAILQ_HEAD(skmhead, "skm_link") :
        format_string = "{:>4d}: 0x{:<08x} {:<4d} {:<4d} {:<4d} {:<4d} {:<4d} {:<4d} {:<4d} {:<4d} {:<4s} \"{:<s}\""
        print format_string.format(i, skm, skm.skm_bufinuse, skm.skm_bufmax, skm.skm_rescale, skm.skm_sl_create, skm.skm_sl_destroy, skm.skm_sl_alloc, skm.skm_sl_free, skm.skm_depot_contention, SkmemCacheModeAsString(skm.skm_mode), str(skm.skm_name))
        i += 1

@lldb_command('showskmemslab')
def ShowBufCtl(cmd_args=None) :
    """ Show slabs and bufctls of a skmem cache
    """

    if (cmd_args == None or len(cmd_args) == 0) :
        print "Missing argument 0 (skmem_cache address)."
        return

    skm = kern.GetValueFromAddress(cmd_args[0], 'skmem_cache *')

    for slab in IterateTAILQ_HEAD(skm.skm_sl_partial, "sl_link") :
        format_string = "{:<18s} {:<4s} {:18s} {:18s}"
        print format_string.format("slab", "ref", "base", "basem")
        format_string = "0x{:<08x} {:<4d} 0x{:<08x} 0x{:08x}"
        print format_string.format(slab, slab.sl_refcnt, slab.sl_base, slab.sl_basem)
        print "\t========================= free ========================="
        format_string = "\t{:<18s} {:18s} {:18s}"
        print format_string.format("bufctl", "buf_addr", "buf_addrm")
        for bc in IterateListEntry(slab.sl_head, 'struct skmem_bufctl *',
          'bc_link', list_prefix='s') :
            format_string = "\t0x{:<08x} 0x{:<08x} 0x{:<08x}"
            print format_string.format(bc, bc.bc_addr, bc.bc_addrm)

    for slab in IterateTAILQ_HEAD(skm.skm_sl_empty, "sl_link") :
        format_string = "{:<18s}  {:<4s} {:18s} {:18s}"
        print format_string.format("slab", "ref", "base", "basem")
        format_string = "0x{:<08x} {:<4d} 0x{:<08x} 0x{:08x}"
        print format_string.format(slab, slab.sl_refcnt, slab.sl_base, slab.sl_basem)
        print "\t========================= free ========================="
        format_string = "\t{:<18s} {:18s} {:18s}"
        print format_string.format("bufctl", "buf_addr", "buf_addrm")
        for bc in IterateListEntry(slab.sl_head, 'struct skmem_bufctl *',
          'bc_link', list_prefix='s') :
            format_string = "\t0x{:<08x} 0x{:<08x} 0x{:<08x}"
            print format_string.format(bc, bc.bc_addr, bc.bc_addrm)

    print " "
    for i in range(0, skm.skm_hash_mask + 1) :
        format_string = "{:<18s}  {:<4s}"
        print format_string.format("bucket", "idx")
        format_string = "0x{:<08x} {:<4d}"
        print format_string.format(addressof(skm.skm_hash_table[i]), i)
        print "\t====================== allocated ======================="
        format_string = "\t{:<18s} {:18s} {:18s}"
        print format_string.format("bufctl", "buf_addr", "buf_addrm")
        for bc in IterateListEntry(skm.skm_hash_table[i].bcb_head,
          'struct skmem_bufctl *', 'bc_link', list_prefix='s') :
            format_string = "\t0x{:<08x} 0x{:<08x} 0x{:<08x}"
            print format_string.format(bc, bc.bc_addr, bc.bc_addrm)

def SkmemArenaTypeAsString(type) :
    out_string = ""
    SKMEM_ARENA_TYPE_NEXUS  = 0
    SKMEM_ARENA_TYPE_NECP   = 1
    SKMEM_ARENA_TYPE_SYSTEM = 2

    if (type == SKMEM_ARENA_TYPE_NEXUS) :
        out_string += "NEXUS"
    elif (type == SKMEM_ARENA_TYPE_NECP) :
        out_string += "NECP"
    elif (type == SKMEM_ARENA_TYPE_SYSTEM) :
        out_string += "SYSTEM"
    else :
        out_string += "?"

    return out_string

@lldb_command('showskmemarena')
def ShowSkmemArena(cmd_args=None) :
    """ Show the global list of skmem arenas
    """

    i = 1
    arhead = kern.globals.skmem_arena_head

    for ar in IterateTAILQ_HEAD(arhead, "ar_link") :
        format_string = "{:>4d}: 0x{:<08x} {:<6s} {:>5d} KB \"{:<s}\""
        print format_string.format(i, ar, SkmemArenaTypeAsString(ar.ar_type), ar.ar_mapsize >> 10, str(ar.ar_name))
        i += 1

@lldb_command('showskmemregions')
def ShowSkmemRegions(cmd_args=None) :
    """ Show the global list of skmem regions
    """

    i = 1
    skrhead = kern.globals.skmem_region_head

    for skr in IterateTAILQ_HEAD(skrhead, "skr_link") :
        format_string = "{:>4d}: 0x{:<08x} \"{:<s}\""
        print format_string.format(i, skr, str(skr.skr_name))
        i += 1

@lldb_command('showskmemregion')
def ShowSkmemRegion(cmd_args=None) :
    """ Show segments of a skmem region
    """

    if (cmd_args == None or len(cmd_args) == 0) :
        print "Missing argument 0 (skmem_region address)."
        return

    skr = kern.GetValueFromAddress(cmd_args[0], 'skmem_region *')

    print "\t========================= free ========================="
    for sg in IterateTAILQ_HEAD(skr.skr_seg_free, "sg_link") :
        format_string = "{:<18s} {:<4s} {:18s} {:18s}"
        print format_string.format("segment", "idx", "start", "end")
        format_string = "0x{:<08x} {:<4d} 0x{:<08x} 0x{:08x}"
        print format_string.format(sg, sg.sg_index, sg.sg_start, sg.sg_end)
        format_string = "\t{:<18s} {:18s} {:18s}"
        print format_string.format("bufctl", "buf_addr", "buf_addrm")

    print " "
    for i in range(0, skr.skr_hash_mask + 1) :
        format_string = "{:<18s}  {:<4s}"
        print format_string.format("bucket", "idx")
        format_string = "0x{:<08x} {:<4d}"
        print format_string.format(addressof(skr.skr_hash_table[i]), i)
        print "\t====================== allocated ======================="
        format_string = "\t{:<18s} {:4s} {:18s} {:18s}"
        print format_string.format("segment", "idx", "start", "end")
        for sg in IterateTAILQ_HEAD(skr.skr_hash_table[i].sgb_head, "sg_link") :
            format_string = "\t0x{:<08x} {:<4d} 0x{:<08x} 0x{:<08x}"
            print format_string.format(sg, sg.sg_index, sg.sg_start, sg.sg_end)

@lldb_command('showchannelupphash')
def ShowChannelUppHash(cmd_args=None) :
    """ Show channel user packet pool hash chain
    """

    if (cmd_args == None or len(cmd_args) == 0) :
        print "Missing argument 0 (skmem_cache address)."
        return

    ch = kern.GetValueFromAddress(cmd_args[0], 'kern_channel *')
    KERN_CHANNEL_UPP_HTBL_SIZE = 256

    for i in range(KERN_CHANNEL_UPP_HTBL_SIZE) :
        bkt = addressof(ch.ch_upp_hash_table[i])
        format_string = "{:>4d} 0x{:<08x}"
        print format_string.format(i, bkt)
        for kqum in IterateListEntry(bkt.upp_head, 'struct __kern_quantum *',
                                      'qum_upp_link', list_prefix='s') :
            format_string = "0x{:<08x}"
            print format_string.format(kqum)

@lldb_type_summary(['struct ns *'])
@header('{:<20s} {:<5s} {:<48s} {:<4s}'.format('ns', 'proto', 'addr', 'nreservations'))
def GetStructNsSummary(ns):
    """ Summarizes a struct ns from the netns

        returns: str - summary of struct ns
    """

    if (ns.ns_proto == IPPROTO_TCP):
        proto = "tcp"
    elif (ns.ns_proto == IPPROTO_UDP):
        proto = "udp"
    else:
        proto = str(ns.ns_proto)

    if (ns.ns_addr_len == sizeof('struct in_addr')):
        addr = GetInAddrAsString(addressof(ns.ns_inaddr))
    elif (ns.ns_addr_len == sizeof('struct in6_addr')):
        addr = GetIn6AddrAsString(ns.ns_in6addr.__u6_addr.__u6_addr8)
    else:
        addr = str(ns_addr) + " bad len {:u}".format(ns.ns_addr_len)

    format_string = '{o:#020x} {p:<5s} {a:<48s} {n:<4d}'

    """ show ports and refs, one per line
    """
    ports_string = "ports & refs\n"
    for f in IterateRBTreeEntry(ns.ns_reservations, 'struct ns_reservation *', 'nsr_link'):
        ports_string += "\t%u" % f.nsr_port
        ports_string += "\tlisten %d\tskywalk %d\tbsd %d\tpf %d\n" % (f.nsr_refs[0], f.nsr_refs[1], f.nsr_refs[2], f.nsr_refs[3])
    """ show just the ports, not refs
    offs = 0
    ports_string = "\nports:\t"
    for f in IterateRBTreeEntry(ns.ns_reservations, 'struct ns_reservation *', 'nsr_link'):
        if (len(ports_string)-offs > 70):
            ports_string += "\n\t"
            offs = len(ports_string)
        ports_string += " %u" % f.nsr_port
    """

    return format_string.format(
        o=ns,
        p=proto,
        a=addr,
        n=ns.ns_n_reservations) + ports_string

@lldb_command('shownetns')
def ShowNetNS(cmd_args=None):
    """ Show the netns table
    """
    print"\nnetns_namespaces:"
    print GetStructNsSummary.header

    namespaces = kern.globals.netns_namespaces
    for ns in IterateRBTreeEntry(namespaces, 'struct ns *', 'ns_link'):
        print GetStructNsSummary(ns)

    print "\nwild: (these should be duplicated above)"
    print GetStructNsSummary.header
    for i in range(0,4):
        print GetStructNsSummary(kern.globals.netns_global_wild[i])

    print "\nnon wild:"
    print GetStructNsSummary.header
    for i in range(0,4):
        print GetStructNsSummary(kern.globals.netns_global_non_wild[i])


@lldb_type_summary(['struct ns_token *'])
@header('{:<20s} {:<5s} {:<48s} {:<12s} {:<8s} {:<38s} {:<38s} {:<12s}'.format('nt', 'proto', 'addr', 'port', 'owner', 'ifp', 'parent', 'flags'))
def GetNsTokenSummary(nt):
    """ Summarizes a struct ns from the netns

        returns: str - summary of struct ns
    """

    if (nt.nt_proto == IPPROTO_TCP):
        proto = "tcp"
    elif (nt.nt_proto == IPPROTO_UDP):
        proto = "udp"
    else:
        proto = str(nt.nt_proto)

    if (nt.nt_addr_len == sizeof('struct in_addr')):
        addr = GetInAddrAsString(addressof(nt.nt_inaddr))
    elif (nt.nt_addr_len == sizeof('struct in6_addr')):
        addr = GetIn6AddrAsString(nt.nt_in6addr.__u6_addr.__u6_addr8)
    else:
        addr = str(nt_addr) + " bad len {:u}".format(nt.nt_addr_len)

    format_string = '{o:#020x} {p:<5s} {a:<48s} {pt:<12s} {wn:<8s} {ifp:38s} {pa:38s} {f:#012x}'

    ports = "%u" % nt.nt_port

    ifp = "(struct ifnet *)" + hex(nt.nt_ifp)

    if ((nt.nt_flags & 0x7) == 0x00):
        owner = "LISTENER"
        parent = "(void *)" + hex(nt.nt_parent)
    elif ((nt.nt_flags & 0x7) == 0x01):
        owner = "SKYWALK"
        parent = "(struct flow_entry *)" + hex(nt.nt_parent_skywalk)
    elif ((nt.nt_flags & 0x7) == 0x02): # XXX xnudefines?
        owner = "BSD"
        parent = "(struct inpcb *)" + hex(nt.nt_parent_bsd)
    elif ((nt.nt_flags & 0x7) == 0x03): # XXX xnudefines?
        owner = "PF"
        parent = "(void *)" + hex(nt.nt_parent)

    return format_string.format(
        o=nt,
        p=proto,
        a=addr,
        pt=ports,
        wn=owner,
        ifp=ifp,
        pa=parent,
        f=nt.nt_flags)

@lldb_command("showallnetnstokens")
def ShowAllNetNSTokens(cmd_args=None):
    """ show all netns tokens
    """

    tokenhead = kern.globals.netns_all_tokens
    print GetNsTokenSummary.header
    for nt in IterateListEntry(tokenhead, 'struct ns_token *', 'nt_all_link', list_prefix='s'):
        print GetNsTokenSummary(nt)

@lldb_command("shownetnstokens")
def ShowNetNSTokens(cmd_args=None):
    """ show netns tokens attached to an ifp
        with no args, shows unbound tokens
    """

    if (cmd_args == None or len(cmd_args) == 0):
        print "No ifp argument provided, showing unbound tokens"
        tokenhead = kern.globals.netns_unbound_tokens
    elif len(cmd_args) > 0:
        ifp = kern.GetValueFromAddress(cmd_args[0], 'ifnet *')
        print "Showing tokens for ifp %r" % ifp
        tokenhead = ifp.if_netns_tokens
    else:
        print "Missing ifp argument 0 in shownetnstokens"
        print cmd_args
        return

    print GetNsTokenSummary.header
    for nt in IterateListEntry(tokenhead, 'struct ns_token *', 'nt_ifp_link', list_prefix='s'):
        print GetNsTokenSummary(nt)

def IterateSTAILQ_HEAD(headval, element_name):
    iter_val = headval.stqh_first
    while unsigned(iter_val) != 0 :
        yield iter_val
        iter_val = iter_val.__getattr__(element_name).stqe_next
    #end of yield loop

@lldb_command("shownexuschannels")
def ShowNexusChannels(cmd_args=None):
    """ show nexus channels
    """
    if (cmd_args == None or len(cmd_args) == 0):
        print "Missing argument 0 (kern_nexus address)."
        return

    nx = kern.GetValueFromAddress(cmd_args[0], 'kern_nexus *')
    i = 1

    format_string = "{:>4s}  {:<18s} {:>4s} {:<7s} {:<7s} {:<18s} {:<18s} {:<18s} {:>8s} {:6s} {:<18s} {:>4s} {:s}"
    print format_string.format("", "addr", "refs", "txrings", "rxrings", "arena", "ioskmap", "mapaddr", "mapsize", "maprdr", "na", "fd", "process")

    for ch in IterateSTAILQ_HEAD(nx.nx_ch_head, "ch_link"):
        format_string = "{:>4d}: 0x{:<08x} {:>4d} [{:2d},{:2d}] [{:2d},{:2d}] 0x{:<08x} 0x{:<08x} 0x{:<16x} {:>8d} {:>6d} 0x{:<08x} {:>4d} {:s}({:d})"
        print format_string.format(i, ch, ch.ch_refcnt, ch.ch_first[0], ch.ch_last[0], ch.ch_first[1], ch.ch_last[1], ch.ch_mmap.ami_arena, ch.ch_mmap.ami_mapref, ch.ch_mmap.ami_mapaddr, ch.ch_mmap.ami_mapsize, ch.ch_mmap.ami_redirect, ch.ch_na, ch.ch_fd, ch.ch_name, ch.ch_pid)
        i += 1

    for ch in IterateSTAILQ_HEAD(nx.nx_ch_nonxref_head, "ch_link"):
        format_string = "{:>4d}: 0x{:<08x} {:>4d} [{:2d},{:2d}] [{:2d},{:2d}] 0x{:<08x} 0x{:<08x} 0x{:<16x} {:>8d} {:>6d} 0x{:<08x} {:>4d} {:s}({:d})"
        print format_string.format(i, ch, ch.ch_refcnt, ch.ch_first[0], ch.ch_last[0], ch.ch_first[1], ch.ch_last[1], ch.ch_mmap.ami_arena, ch.ch_mmap.ami_mapref, ch.ch_mmap.ami_mapaddr, ch.ch_mmap.ami_mapsize, ch.ch_mmap.ami_redirect, ch.ch_na, ch.ch_fd, ch.ch_name, ch.ch_pid)
        i += 1

def IterateProcNECP(proc):
    """ Iterate through all NECP descriptors in the given process

        params:
            proc - the proc object
        returns: nothing, this is meant to be used as a generator function
            necp - yields each necp_fd_data in the process
    """

    proc_filedesc = proc.p_fd
    proc_lastfile = unsigned(proc_filedesc.fd_lastfile)
    proc_ofiles = proc_filedesc.fd_ofiles

    count = 0
    while count <= proc_lastfile:
        if unsigned(proc_ofiles[count]) != 0:
            proc_fd_fglob = proc_ofiles[count].fp_glob
            if (unsigned(proc_fd_fglob.fg_ops.fo_type) == 9):
                yield Cast(proc_fd_fglob.fg_data, 'necp_fd_data *')
        count += 1

def GetNECPClientBitFields(necp):
    """ Return the bit fields in necp_client as string

        returns: str - string representation of necp_client bit fields
    """

    bitfields_string = ''
    if necp.result_read != 0:
        bitfields_string += 'r'
    else:
        bitfields_string += '-'
    if necp.allow_multiple_flows != 0:
        bitfields_string += 'm'
    else:
        bitfields_string += '-'
    if necp.background != 0:
        bitfields_string += 'b'
    else:
        bitfields_string += '-'
    if necp.background_update != 0:
        bitfields_string += 'B'
    else:
        bitfields_string += '-'
    if necp.platform_binary != 0:
        bitfields_string += 'p'
    else:
        bitfields_string += '-'

    return bitfields_string

def GetNECPFlowBitFields(flow_registration):
    """ Return the bit fields in necp_client_flow_registration as string

        returns: str - string representation of necp_client_flow_registration bit fields
    """

    bitfields_string = ''
    if flow_registration.flow_result_read != 0:
        bitfields_string += 'r'
    else:
        bitfields_string += '-'
    if flow_registration.defunct != 0:
        bitfields_string += 'd'
    else:
        bitfields_string += '-'

    return bitfields_string

@lldb_type_summary(['necp_fd_data *'])
@header('{:<20s} {:<8s}'.format('necp_fd_data', "flags"))
def GetNECPSummary(necp):
    """ Summarizes a necp_fd_data and related information

        returns: str - summary of necp_fd_data
    """

    format_string = '{o: <#020x} {u:<#08x}'

    stats_arenas_string = "\n\n\t%-18s %-39s %-4s %-10s\n" % ("stats_arenas", "mmap", "refs", "flags")
    for sa in IterateListEntry(necp.stats_arena_list, 'struct necp_arena_info *', 'nai_chain'):
        stats_arenas_string += "\t0x%016x " % sa
        stats_arenas_string += "[0x%016x-0x%016x) " % (sa.nai_mmap.ami_mapaddr,(sa.nai_mmap.ami_mapaddr+sa.nai_mmap.ami_mapsize))
        stats_arenas_string += "%4u " % sa.nai_use_count
        stats_arenas_string += "0x%08x " % sa.nai_flags
        stats_arenas_string += "\n"

    clients_string = ""
    for c in IterateRBTreeEntry(necp.clients, 'struct necp_client *', 'link'):
        clients_string += "\n\t%-18s %-36s %-4s %-5s\n" % ("necp_clients", "client_id", "refs", "flags")
        clients_string += "\t0x%016x " % c
        clients_string += "%36s " % GetUUIDSummary(c.client_id)
        clients_string += "%4u " % c.reference_count
        clients_string += "%5s " % GetNECPClientBitFields(c)
        count = 0;
        for f in IterateRBTreeEntry(c.flow_registrations, 'struct necp_client_flow_registration *', 'client_link'):
            if count == 0:
                clients_string += "\n\t\t%-18s %-36s %-2s %-18s %-18s %-18s\n" % ("flow_registration", "registraton_id", "flags", "stats_arena", "kstats_obj", "ustats_obj")
            clients_string += "\t\t0x%016x " % f
            clients_string += "%36s " % GetUUIDSummary(f.registration_id)
            clients_string += "%2s " % GetNECPFlowBitFields(f)
            clients_string += "0x%016x " % f.stats_arena
            clients_string += "0x%016x " % f.kstats_kaddr
            clients_string += "0x%016x " % f.ustats_uaddr
        clients_string += "\n"

    return format_string.format(
        o=necp,
        u=necp.flags) + stats_arenas_string + clients_string

@lldb_command('showprocnecp')
def ShowProcNECP(cmd_args=None):
    """ Show NECP descriptors for a given process.

        usage: showprocnecp <proc_t>
    """

    if not cmd_args:
        raise ArgumentError('missing struct proc * argument')

    proc = kern.GetValueFromAddress(cmd_args[0], 'proc_t')

    print GetNECPSummary.header
    for kc in IterateProcNECP(proc):
        print GetNECPSummary(kc)

def NexusTypePtr(nx):
    if nx.nx_prov.nxprov_params.nxp_type == GetEnumValue("nexus_type_t::NEXUS_TYPE_FLOW_SWITCH"):
        return "(struct nx_flowswitch *){:18s}".format(hex(nx.nx_arg))
    elif nx.nx_prov.nxprov_params.nxp_type == GetEnumValue("nexus_type_t::NEXUS_TYPE_NET_IF"):
        return "     (struct nx_netif *){:18s}".format(hex(nx.nx_arg))
    elif nx.nx_prov.nxprov_params.nxp_type == GetEnumValue("nexus_type_t::NEXUS_TYPE_USER_PIPE"):
        return "     (struct nx_upipe *){:18s}".format(hex(nx.nx_arg))
    elif nx.nx_prov.nxprov_params.nxp_type == GetEnumValue("nexus_type_t::NEXUS_TYPE_KERNEL_PIPE"):
        return "   (struct kern_nexus *){:18s}".format(hex(nx))
    else:
        return "unknown"

def GetStructNexusSummary(nx):
    nexus_summary_string = ""
    nexus_summary_string += "{0:s} ".format(NexusTypePtr(nx))
    nexus_summary_string += "{0:30s} ".format(str(Cast(addressof(nx.nx_prov.nxprov_params.nxp_name), 'char *')))
    nexus_summary_string += "rings: tx {:2d} rx {:2d} slots: {:4d} rx {:4d} bufsize {:5d} metasize {:5d} mhints {:2d} ".format(
            nx.nx_prov.nxprov_params.nxp_tx_rings,
            nx.nx_prov.nxprov_params.nxp_rx_rings,
            nx.nx_prov.nxprov_params.nxp_rx_slots,
            nx.nx_prov.nxprov_params.nxp_tx_slots,
            nx.nx_prov.nxprov_params.nxp_buf_size,
            nx.nx_prov.nxprov_params.nxp_meta_size,
            nx.nx_prov.nxprov_params.nxp_mhints)

    return nexus_summary_string

@lldb_command('shownexuses')
def ShowNexuses(cmd_args=None):
    """ Show Nexus.

        usage: shownexues
    """
    nexus_summaries = []
    nexuses = kern.globals.nx_head
    for nx in IterateRBTreeEntry(nexuses, 'struct kern_nexus*', 'nx_link'):
        nexus_summaries.append(GetStructNexusSummary(nx))
    nexus_summaries.sort()
    for nx_str in nexus_summaries:
        print "{0:s}".format(nx_str)

def GetSockAddr4(in_addr):
    return inet_ntoa(struct.pack("!I", in_addr.sin_addr))

def GetSockAddr6(in6_addr):
    addr = in6_addr.__u6_addr.__u6_addr8
    addr_raw_string = ":".join(["{0:02x}{0:02x}".format(unsigned(addr[i]),
        unsigned(addr[i+1])) for i in range(0, 16, 2)])
    return inet_ntop(AF_INET6, inet_pton(AF_INET6, addr_raw_string))

def FlowKeyStr(fk):
    if fk.fk_ipver == 0x4:
        src_str = GetSockAddr4(fk.fk_src._v4)
        dst_str = GetSockAddr4(fk.fk_dst._v4)
    elif fk.fk_ipver == 0x60:
        src_str = GetSockAddr6(fk.fk_src._v6)
        dst_str = GetSockAddr6(fk.fk_dst._v6)
    else:
        return "unkown ipver"

    return "src={},dst={},proto={},sport={},dport={}".format(src_str, dst_str,
            unsigned(fk.fk_proto), ntohs(fk.fk_sport), ntohs(fk.fk_dport))

def FlowEntryStr(fe):
    return "(struct flow_entry*){} {} ".format(hex(fe), FlowKeyStr(fe.fe_key))

def GetFlowEntryPid(fe):
    return fe.fe_pid

def GetFlowswitchFlowEntries(fsw):
    fm = kern.GetValueFromAddress(unsigned(fsw.fsw_flow_mgr), 'struct flow_mgr *')
    cht = kern.GetValueFromAddress(unsigned(fm.fm_flow_table), 'struct cuckoo_hashtable *')

    flows = []
    def GetCuckooNodeAsFLowEntry(node, hashValue):
            fe = containerof(node, 'struct flow_entry', 'fe_cnode')
            flows.append(fe)

    CuckooHashtableForeach(cht, GetCuckooNodeAsFLowEntry)
    return flows

def IsNexusAFlowswitch(nx):
    return nx.nx_prov.nxprov_params.nxp_type == GetEnumValue('nexus_type_t::NEXUS_TYPE_FLOW_SWITCH')

def GetNexusAsFlowswitch(nx):
    return kern.GetValueFromAddress(unsigned(nx.nx_arg), 'struct nx_flowswitch *')

def FlowswitchStr(fsw):
    return "{}:\n(struct nx_flowswitch *){}".format(str(fsw.fsw_ifp.if_xname), hex(fsw))

@lldb_command('showflowswitches')
def ShowFlowswitches(cmd_args=None):
    """ Show flow switches

        usage: showflowswitches [ifname]
    """
    ifname = ""
    if len(cmd_args) == 1:
        ifname = cmd_args[0]

    nexuses = kern.globals.nx_head
    for nx in IterateRBTreeEntry(nexuses, 'struct kern_nexus*', 'nx_link'):
        if not IsNexusAFlowswitch(nx):
            continue
        fsw = GetNexusAsFlowswitch(nx)
        if ifname not in str(fsw.fsw_ifp.if_xname):
            continue
        print "{}".format(FlowswitchStr(fsw))
        flows = GetFlowswitchFlowEntries(fsw)
        flows.sort(key=GetFlowEntryPid)
        for fe in flows:
            print "    {}".format(FlowEntryStr(fe))

def CuckooHashtableForeachSlot(cht, slotHandler):
    for i in range(0, cht._n_buckets):
        b = cht._buckets[i]
        if unsigned(b._inuse) == 0:
            continue
        for j in range(0, kern.globals._CHT_BUCKET_SLOTS):
            s = b._slots[j]
            if unsigned(s._node) != 0:
                slotHandler(s)

def CuckooHashtableForeach(cht, handler):
    def CuckooHashtableSlotHandler(s):
        if unsigned(s._node) == 0:
            return
        node = s._node
        while unsigned(node) != 0:
            handler(node, s._hash)
            node = node.next
    CuckooHashtableForeachSlot(cht, CuckooHashtableSlotHandler)

@lldb_command('showcuckoohashtable')
def ShowCuckooHashtable(cmd_args=None):
    """ Show Cuckoo Hashtable.

        usage: showcuckoohashtable <struct cuckoo_hashtable *>
    """
    if not cmd_args:
        raise ArgumentError('missing struct cuckoo_hashtable * argument')

    cht = kern.GetValueFromAddress(cmd_args[0], 'struct cuckoo_hashtable *')

    print "(struct cuckoo_hashtable *){:18s} capacity {:d} entries {:d}".format(hex(cht), cht._capacity, cht._n_entries)
    def CuckooHashtablePrintNode(node, hashValue):
        print "  node {} hash 0x{:08x}".format(hex(node), int(hashValue))

    CuckooHashtableForeach(cht, CuckooHashtablePrintNode)

@lldb_command('showprotons')
def ShowProtoNS(cmd_args=None):
    """ Show the protons table
    """

    protons_tokens = kern.globals.protons_tokens
    for pt in IterateRBTreeEntry(protons_tokens, 'struct protons_token *', 'pt_link'):
        print "(struct protons_token *){} protocol {:3} pid {:5} epid {:5} ref {:2} flags {}".format(
                hex(pt), int(pt.pt_protocol), int(pt.pt_pid), int(pt.pt_epid),
                int(pt.pt_refcnt.ref_count), hex(pt.pt_flags))
