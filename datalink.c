#include <stdio.h>
#include <string.h>
#include<stdbool.h>

#include "protocol.h"
#include "datalink.h"
#define MAX_SEQ 7
#define DATA_TIMER  2000

typedef struct FRAME { 
    unsigned char kind; /* FRAME_DATA */
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN]; 
    unsigned int  padding;
}frame;

typedef struct {
    unsigned char data[PKT_LEN];
}packet;

typedef unsigned char seq_nr;

packet buffer[MAX_SEQ+1];   //数据包缓存
seq_nr nbuffered = 0;     //发送窗口长度
seq_nr frame_expected = 0;    //接受窗口希望的帧
seq_nr ack_expected = 0;      //希望ack的帧
seq_nr next_frame_to_send = 0;    //将要发送的帧
static int phl_ready = 0;   //物理层是否准备好

static bool between(seq_nr a, seq_nr b, seq_nr c)
{
    return (( a<=b )&&( b<c )) || (( c<a )&&( a<=b )) || (( b<c )&&( c<a )); 
}

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_data_frame(seq_nr frame_nr, seq_nr frame_expected, packet buffer[])
{
    frame s;

    s.kind = FRAME_DATA;
    s.seq = frame_nr;
    s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
    memcpy(s.data, buffer[frame_nr].data, PKT_LEN);

    dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

    put_frame((unsigned char *)&s, 3 + PKT_LEN);
    start_timer(frame_nr, DATA_TIMER);
}

static void inc(k)
{
    if(k < MAX_SEQ)
        k = k + 1;
    else 
        k = 0;
}

int main(int argc, char **argv)
{
    int event, arg;
    frame f;
    int len = 0;

    protocol_init(argc, argv); 
    lprintf("Designed by Jiang Yanjun, build: " __DATE__"  "__TIME__"\n");

    disable_network_layer();

    for (;;) {
        event = wait_for_event(&arg);

        switch (event) {
        case NETWORK_LAYER_READY:
            get_packet(buffer[next_frame_to_send].data);
            nbuffered++;
            send_data_frame(next_frame_to_send, frame_expected, &buffer);
            inc(next_frame_to_send);
            break;

        case PHYSICAL_LAYER_READY:
            phl_ready = 1;
            break;

        case FRAME_RECEIVED: 
            len = recv_frame((unsigned char *)&f, sizeof f);
            if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
                dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                break;
            }
            if (f.kind == FRAME_DATA) {
                dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
                if (f.seq == frame_expected) {
                    put_packet(f.data, len - 7);
                    inc(frame_expected);
                }
            } 
            while(between(ack_expected, f.ack, next_frame_to_send))
            {
                nbuffered--;
                stop_timer(ack_expected);
                inc(ack_expected);
            }
            break; 

        case DATA_TIMEOUT:
            dbg_event("---- DATA %d timeout\n", arg); 
            next_frame_to_send = ack_expected;
            for(int i = 1; i <= nbuffered; i++)
            {
                send_data_frame(next_frame_to_send, frame_expected, &buffer);
                inc(next_frame_to_send);
            }
            break;
        }

        if ((nbuffered < MAX_SEQ) && phl_ready)
            enable_network_layer();
        else
            disable_network_layer();
   }
}
