/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Singleton.h>
#include <AK/StringBuilder.h>
#include <Kernel/Arch/x86/InterruptDisabler.h>
#include <Kernel/Heap/kmalloc.h>
#include <Kernel/Lock.h>
#include <Kernel/Net/EtherType.h>
#include <Kernel/Net/LoopbackAdapter.h>
#include <Kernel/Net/NetworkAdapter.h>
#include <Kernel/Net/NetworkingManagement.h>
#include <Kernel/Process.h>
#include <Kernel/Random.h>
#include <Kernel/StdLib.h>

namespace Kernel {

NetworkAdapter::NetworkAdapter()
{
}

NetworkAdapter::~NetworkAdapter()
{
}

void NetworkAdapter::send_packet(ReadonlyBytes packet)
{
    m_packets_out++;
    m_bytes_out += packet.size();
    send_raw(packet);
}

void NetworkAdapter::send(const MACAddress& destination, const ARPPacket& packet)
{
    size_t size_in_bytes = sizeof(EthernetFrameHeader) + sizeof(ARPPacket);
    auto buffer = NetworkByteBuffer::create_zeroed(size_in_bytes);
    auto* eth = (EthernetFrameHeader*)buffer.data();
    eth->set_source(mac_address());
    eth->set_destination(destination);
    eth->set_ether_type(EtherType::ARP);
    memcpy(eth->payload(), &packet, sizeof(ARPPacket));
    send_packet({ (const u8*)eth, size_in_bytes });
}

void NetworkAdapter::fill_in_ipv4_header(PacketWithTimestamp& packet, IPv4Address const& source_ipv4, MACAddress const& destination_mac, IPv4Address const& destination_ipv4, IPv4Protocol protocol, size_t payload_size, u8 ttl)
{
    size_t ipv4_packet_size = sizeof(IPv4Packet) + payload_size;
    VERIFY(ipv4_packet_size <= mtu());

    size_t ethernet_frame_size = ipv4_payload_offset() + payload_size;
    VERIFY(packet.buffer.size() == ethernet_frame_size);
    memset(packet.buffer.data(), 0, ipv4_payload_offset());
    auto& eth = *(EthernetFrameHeader*)packet.buffer.data();
    eth.set_source(mac_address());
    eth.set_destination(destination_mac);
    eth.set_ether_type(EtherType::IPv4);
    auto& ipv4 = *(IPv4Packet*)eth.payload();
    ipv4.set_version(4);
    ipv4.set_internet_header_length(5);
    ipv4.set_source(source_ipv4);
    ipv4.set_destination(destination_ipv4);
    ipv4.set_protocol((u8)protocol);
    ipv4.set_length(sizeof(IPv4Packet) + payload_size);
    ipv4.set_ident(1);
    ipv4.set_ttl(ttl);
    ipv4.set_checksum(ipv4.compute_checksum());
}

void NetworkAdapter::did_receive(ReadonlyBytes payload)
{
    InterruptDisabler disabler;
    m_packets_in++;
    m_bytes_in += payload.size();

    if (m_packet_queue_size == max_packet_buffers) {
        // FIXME: Keep track of the number of dropped packets
        return;
    }

    auto packet = acquire_packet_buffer(payload.size());
    if (!packet) {
        dbgln("Discarding packet because we're out of memory");
        return;
    }

    memcpy(packet->buffer.data(), payload.data(), payload.size());

    m_packet_queue.append(*packet);
    m_packet_queue_size++;

    if (on_receive)
        on_receive();
}

size_t NetworkAdapter::dequeue_packet(u8* buffer, size_t buffer_size, Time& packet_timestamp)
{
    InterruptDisabler disabler;
    if (m_packet_queue.is_empty())
        return 0;
    auto packet_with_timestamp = m_packet_queue.take_first();
    m_packet_queue_size--;
    packet_timestamp = packet_with_timestamp->timestamp;
    auto& packet_buffer = packet_with_timestamp->buffer;
    size_t packet_size = packet_buffer.size();
    VERIFY(packet_size <= buffer_size);
    memcpy(buffer, packet_buffer.data(), packet_size);
    release_packet_buffer(*packet_with_timestamp);
    return packet_size;
}

RefPtr<PacketWithTimestamp> NetworkAdapter::acquire_packet_buffer(size_t size)
{
    InterruptDisabler disabler;
    if (m_unused_packets.is_empty()) {
        auto buffer = KBuffer::create_with_size(size, Region::Access::Read | Region::Access::Write, "Packet Buffer", AllocationStrategy::AllocateNow);
        auto packet = adopt_ref_if_nonnull(new PacketWithTimestamp { move(buffer), kgettimeofday() });
        if (!packet)
            return nullptr;
        packet->buffer.set_size(size);
        return packet;
    }

    auto packet = m_unused_packets.take_first();
    if (packet->buffer.capacity() >= size) {
        packet->timestamp = kgettimeofday();
        packet->buffer.set_size(size);
        return packet;
    }

    auto buffer = KBuffer::create_with_size(size, Region::Access::Read | Region::Access::Write, "Packet Buffer", AllocationStrategy::AllocateNow);
    packet = adopt_ref_if_nonnull(new PacketWithTimestamp { move(buffer), kgettimeofday() });
    if (!packet)
        return nullptr;
    packet->buffer.set_size(size);
    return packet;
}

void NetworkAdapter::release_packet_buffer(PacketWithTimestamp& packet)
{
    InterruptDisabler disabler;
    m_unused_packets.append(packet);
}

void NetworkAdapter::set_ipv4_address(const IPv4Address& address)
{
    m_ipv4_address = address;
}

void NetworkAdapter::set_ipv4_netmask(const IPv4Address& netmask)
{
    m_ipv4_netmask = netmask;
}

void NetworkAdapter::set_ipv4_gateway(const IPv4Address& gateway)
{
    m_ipv4_gateway = gateway;
}

void NetworkAdapter::set_interface_name(const PCI::Address& pci_address)
{
    // Note: This stands for e - "Ethernet", p - "Port" as for PCI bus, "s" for slot as for PCI slot
    auto name = String::formatted("ep{}s{}", pci_address.bus(), pci_address.device());
    VERIFY(!NetworkingManagement::the().lookup_by_name(name));
    m_name = move(name);
}

void NetworkAdapter::set_loopback_name()
{
    auto name = String("loop");
    VERIFY(!NetworkingManagement::the().lookup_by_name(name));
    m_name = move(name);
}
}
