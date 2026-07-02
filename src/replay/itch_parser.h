#pragma once
// =============================================================================
// itch_parser.h  —  NASDAQ ITCH 5.0 Binary Protocol Parser
//
// Real matching engines don't receive JSON or REST API calls. They receive
// packed binary structs over UDP multicast or TCP. 
// 
// NASDAQ uses the ITCH 5.0 protocol. Data is sent Big-Endian, so we must
// byte-swap it on standard x86 Little-Endian architectures.
//
// This parser reads raw ITCH 5.0 messages (prefixed by a 2-byte length, as
// seen in SoupBinTCP or standard historical PCAP dumps) and translates them
// into our internal `Order` struct.
// =============================================================================

#include "orderbook/types.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <fstream>
#include <iostream>

// ─── Byte Swapping (Network Big-Endian to Host Little-Endian) ───────────────
inline uint16_t bswap16(uint16_t x) { 
    return (x >> 8) | (x << 8); 
}
inline uint32_t bswap32(uint32_t x) {
    return ((x >> 24) & 0xff) | ((x << 8) & 0xff0000) | 
           ((x >> 8) & 0xff00) | ((x << 24) & 0xff000000);
}
inline uint64_t bswap64(uint64_t x) {
    return ((x & 0x00000000000000FFULL) << 56) | 
           ((x & 0x000000000000FF00ULL) << 40) | 
           ((x & 0x0000000000FF0000ULL) << 24) | 
           ((x & 0x00000000FF000000ULL) <<  8) | 
           ((x & 0x000000FF00000000ULL) >>  8) | 
           ((x & 0x0000FF0000000000ULL) >> 24) | 
           ((x & 0x00FF000000000000ULL) >> 40) | 
           ((x & 0xFF00000000000000ULL) >> 56);
}

// ─── ITCH 5.0 Message Layouts ─────────────────────────────────────────────────
#pragma pack(push, 1) // Force 1-byte alignment to match raw network packets

// Message Type 'A' (Add Order)
// Total length: 36 bytes
struct ItchAddOrder {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp;      // Nanoseconds since midnight
    uint64_t order_ref_num;
    char     buy_sell_indicator; // 'B' = Buy, 'S' = Sell
    uint32_t shares;
    char     stock[8];
    uint32_t price;          // Integer price (Implied 4 decimal places, e.g. 1000000 = $100.0000)
};

#pragma pack(pop)

class ItchParser {
public:
    static std::vector<std::shared_ptr<Order>> parse_file(const char* filepath) {
        std::vector<std::shared_ptr<Order>> orders;
        std::ifstream file(filepath, std::ios::binary);
        if (!file) {
            std::cerr << "Failed to open ITCH file: " << filepath << std::endl;
            return orders;
        }

        while (file.peek() != EOF) {
            // 1. Read 2-byte message length prefix
            uint16_t msg_length;
            if (!file.read(reinterpret_cast<char*>(&msg_length), 2)) break;
            msg_length = bswap16(msg_length);
            
            // 2. Read 1-byte message type
            char msg_type;
            if (!file.read(&msg_type, 1)) break;
            
            // 3. Read the rest of the payload
            std::vector<char> payload(msg_length - 1);
            if (!file.read(payload.data(), msg_length - 1)) break;
            
            // 4. Parse specific message types
            if (msg_type == 'A') {
                if (payload.size() < sizeof(ItchAddOrder)) continue;
                
                ItchAddOrder* msg = reinterpret_cast<ItchAddOrder*>(payload.data());
                
                auto o = global_order_pool.acquire();
                // Byte swap network data to host native
                o->order_id     = bswap64(msg->order_ref_num);
                o->trader_id    = 0; // ITCH public feeds mask the actual trader IDs
                o->timestamp_ns = bswap64(msg->timestamp);
                o->side         = (msg->buy_sell_indicator == 'B') ? Side::BUY : Side::SELL;
                o->type         = OrderType::LIMIT;
                o->quantity     = bswap32(msg->shares);
                o->status       = OrderStatus::NEW;
                
                // ITCH prices have 4 implied decimal places
                o->price = static_cast<double>(bswap32(msg->price)) / 10000.0;
                
                // Stock strings in ITCH are space-padded
                char sym[9] = {};
                for (int i = 0; i < 8; i++) {
                    if (msg->stock[i] != ' ') sym[i] = msg->stock[i];
                    else break;
                }
                o->set_symbol(sym);
                
                orders.push_back(o);
            } 
            // In a full implementation, we'd handle 'X' (Cancel), 'D' (Delete), 'E' (Execute), etc.
        }
        return orders;
    }
};
