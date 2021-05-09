#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER  3000  //数据计时器
#define ACK_TIMER 1000            //ACK计时器
#define MAX_SEQ 31
#define NR_BUFS ((MAX_SEQ+1)/2)

struct FRAME {
    unsigned char kind; // FRAME_DATA ，FRAME_NAK ,FRAME_ACK
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN];
    unsigned int  padding;
};

bool arrive[NR_BUFS];//标记接收缓冲区是否被占用
bool no_nak = true;//没有nak发送
static unsigned char  nbuffered = 0;//当前发送缓存数目
static unsigned char in_buffer[NR_BUFS][PKT_LEN], out_buffer[NR_BUFS][PKT_LEN];//接收，发送缓存
static int phl_ready = 0;//物理层是否准备好
static unsigned char ack_expected = 0, next_frame_to_send = 0;//发送窗口下界和上界
static unsigned char  frame_expected = 0, too_far = NR_BUFS;//接收窗口下界和上界

static void inc(unsigned char* k)
{
    if(*k < MAX_SEQ)
        (*k)++;
    else
        *k = 0;
}

static bool between(unsigned char a, unsigned char b, unsigned char c)
{
    return (((a <= b) && (b < c)) || ((c < a) && (a <= b)) || ((b < c) && (c < a)));
}

static void put_frame(unsigned char *frame, int len)
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phl_ready = 0;
}

static void send_data_frame(unsigned char F_kind, unsigned char frame_nr)//发送包的函数，可能是DATA 或ACK 或NAK
{
    struct FRAME s;

    s.kind = F_kind;
    s.seq = frame_nr;
    s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
    switch (F_kind)
    {
    case FRAME_DATA:
    {
        for (int i = 0; i < PKT_LEN; i++)
            s.data[i] = out_buffer[frame_nr%NR_BUFS][i];


        dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

        put_frame((unsigned char *)&s, 3 + PKT_LEN);
        start_timer(frame_nr % NR_BUFS, DATA_TIMER);
    }break;
    case FRAME_ACK:
    {
        dbg_frame("Send ACK %d\n", s.ack);
        put_frame((unsigned char *)&s, 2);

    }break;
    case FRAME_NAK:
    {
        no_nak = false;
        dbg_frame("Send NAK %d\n", s.ack);
        put_frame((unsigned char *)&s, 2);
    }break;
    }
    //phl_ready = 0; 只是一帧的发送需要准备好  同时在流量控制也有用
    stop_ack_timer();   //发送数据时候就把ACK定时器给关闭
}


int main(int argc, char **argv)
{
    int event, arg;
    struct FRAME f;
    int len = 0;
    for (int i = 0; i < NR_BUFS; i++)
        arrive[i] = false;

    protocol_init(argc, argv);
    lprintf("Designed by Jiang Yanjun, build: " __DATE__"  "__TIME__"\n");

    enable_network_layer();

    for (;;)
    {
        event = wait_for_event(&arg);

        switch (event)
        {
            case NETWORK_LAYER_READY://网络层准备好
            {
                dbg_event("Nerwork layer ready:\n");
                get_packet(out_buffer[next_frame_to_send % NR_BUFS]);//从网络层拿包
                nbuffered++;//缓存+1
                send_data_frame(FRAME_DATA, next_frame_to_send);//发包
                next_frame_to_send++;
                next_frame_to_send = next_frame_to_send % (MAX_SEQ + 1);
                break;
            }
            case PHYSICAL_LAYER_READY://物理层 准备好
            {
                dbg_event("Physical layer ready:\n");
                phl_ready = 1;//更改状态
                break;
            }
            case FRAME_RECEIVED://收到一个包
            {
                dbg_event("Frame had received:\n");
                len = recv_frame((unsigned char *)&f, sizeof f);        //校验CRC
                if (len < 5 || crc32((unsigned char *)&f, len) != 0)
                {
                    dbg_event("**** Receiver Error, Bad CRC Checksum\n");
                    if (no_nak)
                        send_data_frame(FRAME_NAK, 0);
                    break;
                }
                
                if (f.kind == FRAME_DATA)
                {
                    dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
                    if (f.seq != frame_expected && no_nak)      //序号不对而且还没发NAK
                    {
                        send_data_frame(FRAME_NAK, 0);
                    }
                    else
                        start_ack_timer(ACK_TIMER);     //启动ACK定时器
                    if (between(frame_expected, f.seq, too_far) && (arrive[f.seq % NR_BUFS]) == false)      //包正确
                    {
                        arrive[f.seq % NR_BUFS] = true;         //标记状态要更改
                        for (int i = 0; i < len - 7; i++)
                            in_buffer[f.seq % NR_BUFS][i] = f.data[i];      //放到接收缓冲区
                        while (arrive[frame_expected%NR_BUFS])      //对有标记状态的包按顺序上交给网络层
                        {
                            put_packet(in_buffer[frame_expected%NR_BUFS], len - 7);
                            no_nak = true;
                            arrive[frame_expected % NR_BUFS] = false;
                            inc(&frame_expected);
                            inc(&too_far);
                            start_ack_timer(ACK_TIMER);
                        }
                    }
                }

                if (f.kind == FRAME_NAK && between(ack_expected, (f.ack + 1) % (MAX_SEQ + 1), next_frame_to_send))
                {
                    dbg_frame("Recv NAK  %d\n", f.ack);
                    send_data_frame(FRAME_DATA, (f.ack + 1) % (MAX_SEQ + 1));       //收到NAK，重发对应该帧
                }
                
                while (between(ack_expected, f.ack, next_frame_to_send))        //收到ACK  停止包计时器
                {
                    nbuffered--;
                    stop_timer(ack_expected % NR_BUFS);
                    inc(&ack_expected);
                }
                break;
            }
            case DATA_TIMEOUT:      //数据包超时  需要重传
            {
                dbg_event("---- DATA %d timeout\n", arg);
                send_data_frame(FRAME_DATA, ack_expected);
                break;
            }
            case ACK_TIMEOUT:       //ack超时  重新发送ACK
            {
                dbg_event("ACK %d timeout\n", arg);
                send_data_frame(FRAME_ACK, 0);
                break;
            }
        }

        if (nbuffered < NR_BUFS && phl_ready)          //确定好物理层状态在产生事件
            enable_network_layer();
        else
            disable_network_layer();
    }
}