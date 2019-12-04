#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER   5000
#define ACK_TIMER    1000
#define MAX_SEQ      127
#define WIN_BUF      64

typedef unsigned char byte;
typedef unsigned short int_16;

// use byte type to avoid memory align issues
typedef struct data_frame {
    byte head[2];            /* KIND(2bits) | SEQ(7bits) | ACK(7bits) */
    byte data[PKT_LEN];
    byte crc[4];
} data_frame;

typedef struct ctrl_frame {
    byte head[2];            /* KIND(2bits) | STUFF(7bits) | ACK/NAK(7bits) */
    byte crc[4];
} ctrl_frame;

inline int get_frame_type(byte *p) {
    return (*(int_16 *)p & 0xC000) >> 14;
}
inline int get_frame_seq(byte *p) {
    return (*(int_16 *)p & 0x3F80) >> 7;
}
inline int get_frame_ack(byte *p) {
    return *(int_16 *)p & 0x7F;
}
inline int get_frame_nak(byte *p) {
    return get_frame_ack(p);
}
inline void set_frame_type(byte *p, int_16 type) {
    *(int_16 *)p &= ~0xC000;
    *(int_16 *)p |= type << 14;
}
inline void set_frame_seq(byte *p, int_16 seq) {
    *(int_16 *)p &= ~0x3F80;
    *(int_16 *)p |= seq << 7;
}
inline void set_frame_ack(byte *p, int_16 ack) {
    *(int_16 *)p &= ~0x007F;
    *(int_16 *)p |= ack;
}
inline void set_frame_nak(byte *p, int_16 nak) {
    set_frame_ack(p, nak);
}


// reciver variables
data_frame recv_window[WIN_BUF];
int recv_l = 0, recv_r = WIN_BUF;
int pending_ack = -1;
int arrived[WIN_BUF] = {0};
byte recv_buffer[PKT_LEN + 10];

// sender variables
data_frame send_window[WIN_BUF];
int send_l = 0, send_r = 0;
int send_buffer_num = 0;
int nak_sent[WIN_BUF] = {0};
byte data_buffer[PKT_LEN + 10];

// auxiliary variables
int phl_ready = 0;


inline int in_send_window(int pos) {
    return (send_l <= send_r && send_l <= pos && pos < send_r)
           || (send_r < send_l && (send_l <= pos || pos < send_r));
}
inline int in_recv_window(int pos) {
    return (recv_l <= recv_r && recv_l <= pos && pos < recv_r)
           || (recv_r < recv_l && (recv_l <= pos || pos < recv_r));
}
inline void inc_border(int *pos) {
    *pos = (*pos + 1) % (MAX_SEQ + 1);
}

void add_send_frame(int pos) {
    // pack a data frame from data_buffer and put it into send window
    data_frame *p = &send_window[pos];

    set_frame_type(p->head, FRAME_DATA);
    set_frame_seq(p->head, send_r);
    memcpy(p->data, data_buffer, PKT_LEN);
}

void send_data_frame(int pos) {
    // fetch a frame from send window and send it

    data_frame *p = &send_window[pos];

    if (pending_ack != -1) {
        set_frame_ack(p->head, pending_ack);
        pending_ack = -1;
        stop_ack_timer();
    } else {
        set_frame_ack(p->head, (recv_l + MAX_SEQ) % (MAX_SEQ + 1));
    }

    dbg_frame("Send DATA %d %d, ID %d\n", get_frame_seq(p->head),
                                    get_frame_ack(p->head), *(short *)p->data);
    
    *(unsigned int *)p->crc = crc32((byte *)p, 2 + PKT_LEN);
    
    send_frame((byte *)p, 2 + PKT_LEN + 4);
    start_timer(get_frame_seq(p->head), DATA_TIMER);

    phl_ready = 0;
}

void send_ack_frame(int ack) {
    ctrl_frame f;

    set_frame_type(f.head, FRAME_ACK);
    set_frame_ack(f.head, ack);
    *(unsigned int *)f.crc = crc32((byte *)&f, 2);

    dbg_frame("Send ACK  %d\n", ack);

    send_frame((byte *)&f, 6);
}

void send_nak_frame(int nak) {
    ctrl_frame f;

    set_frame_type(f.head, FRAME_NAK);
    set_frame_nak(f.head, nak);
    *(unsigned int *)f.crc = crc32((byte *)&f, 2);

    dbg_frame("Send NAK  %d\n", nak);

    send_frame((byte *)&f, 6);
}

void process_ack(int ack) {
    dbg_frame("Recv ACK  %d\n", ack);
    if (in_send_window(ack)) {
        for (; in_send_window(ack); inc_border(&send_l)) {
            send_buffer_num--;
            stop_timer(send_l);
        }
    }
}


int main(int argc, char **argv)
{
    int event, arg;
    int len = 0;

    protocol_init(argc, argv);
    lprintf("Designed by Liu Siyao, build: " __DATE__"  "__TIME__"\n");

    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {

        case NETWORK_LAYER_READY:
            get_packet(data_buffer);
            add_send_frame(send_r % WIN_BUF);
            send_data_frame(send_r % WIN_BUF);
            inc_border(&send_r);
            send_buffer_num++;
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        case FRAME_RECEIVED:
            len = recv_frame(recv_buffer, sizeof recv_buffer);
            if (len < 6 || crc32(recv_buffer, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;
            }

            if (get_frame_type(recv_buffer) == FRAME_DATA) {

                process_ack(get_frame_ack(recv_buffer));

                int seq = get_frame_seq(recv_buffer);
                if (!in_recv_window(seq)) break;
                recv_window[seq % WIN_BUF] = *(data_frame *)recv_buffer;
                arrived[seq % WIN_BUF] = 1;
                dbg_frame(" - Recv DATA %d %d, ID %d\n", 
                          get_frame_seq(recv_buffer), 
                          get_frame_ack(recv_buffer),
                          *(short *)recv_window[seq % WIN_BUF].data);

                if (seq != recv_l) {
                    if (!nak_sent[recv_l % WIN_BUF]) {
                        send_nak_frame(recv_l % WIN_BUF);
                        nak_sent[recv_l % WIN_BUF] = 1;
                    }
                } else {
                    for (; arrived[recv_l % WIN_BUF]; inc_border(&recv_l)) {
                        inc_border(&recv_r);
                        arrived[recv_l % WIN_BUF] = 0;
                        nak_sent[recv_l % WIN_BUF] = 0;
                        put_packet(recv_window[recv_l % WIN_BUF].data, PKT_LEN);
                        pending_ack = recv_l;
                    }
                    start_ack_timer(ACK_TIMER);
                }

            } else if (get_frame_type(recv_buffer) == FRAME_ACK) {

                process_ack(get_frame_ack(recv_buffer));

            } else if (get_frame_type(recv_buffer) == FRAME_NAK) {

                dbg_frame("Recv NAK  %d\n", get_frame_nak(recv_buffer));
                send_data_frame(get_frame_nak(recv_buffer) % WIN_BUF);

            } else {
                
                dbg_event("**** Receiver Error, Unknown Frame Type\n");

            }

            break;

        case DATA_TIMEOUT:
    
            dbg_event("---- DATA %d timeout\n", arg);
            send_data_frame(arg % WIN_BUF);

            break;

        case ACK_TIMEOUT:
        
            dbg_event("---- ACK %d timeout\n", arg);
            send_ack_frame(pending_ack);
            pending_ack = -1;
            break;

        }

        if (send_buffer_num < WIN_BUF && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
   }

}
