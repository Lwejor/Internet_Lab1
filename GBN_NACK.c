#include <stdio.h>
#include <string.h>
 
#include "protocol.h"
#include "datalink.h"
#define MAX_SEQ 6
#define DATA_TIMER  2600
 
#define inc(k) if(k<MAX_SEQ) k=k + 1;else k = 0
typedef struct								//定义数据包packet 
{
	unsigned char data[PKT_LEN];
}	packet;   
typedef enum 
{
	true,false
} boolean ;
typedef unsigned int seq_nr ;
typedef struct								//定义帧frame
{ 
    unsigned char kind;						// FRAME_DATA 
    unsigned char ack;
    unsigned char seq;
    unsigned char data[PKT_LEN]; 
    unsigned int  padding;
} frame ;
static int phylsical_ready = 0;						//物理层是否准备好的标志，为1表示物理层已准备好，为0表示物理层没准备好.
seq_nr next_frame_to_send=0;
seq_nr ack_expected=0;
seq_nr frame_expected=0;
seq_nr nbuffered = 0;				//正在用的缓存
frame  f;
seq_nr i;		
packet buffer[MAX_SEQ+1];			
int event,arg,len;					//事件，帧长度
static boolean between(seq_nr a, seq_nr b, seq_nr c)
{
	return (( a<=b )&&( b<c )) || (( c<a )&&( a<=b )) || (( b<c )&&( c<a )) ;
}
static void put_frame(unsigned char *frame, int len)					//加入校验和
{
    *(unsigned int *)(frame + len) = crc32(frame, len);
    send_frame(frame, len + 4);
    phylsical_ready = 0;
}
static void send_data(seq_nr frame_nr,seq_nr frame_expected,packet buffer[])
{
	frame f;
	f.kind = FRAME_DATA;										//帧的类型
	f.seq = frame_nr;										//给帧加序号
	f.ack = ( frame_expected + MAX_SEQ ) % ( MAX_SEQ + 1 ) ;					//捎带确认
	memcpy(f.data, buffer[frame_nr].data, PKT_LEN);							//将buffer[frame_nr]中的数据拷贝到s.data中
	
	dbg_frame("Send DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
	put_frame( (unsigned char *)&f, 3 + PKT_LEN );									 //传输帧
	start_timer(frame_nr, DATA_TIMER);									   	//启动计时器   
}
int main(int argc,char **argv)
{
	protocol_init(argc, argv);
	lprintf("Designed by nancyyihao, build: " __DATE__"  "__TIME__"\n");
	disable_network_layer();
	while(1)	
	{
		
		event = wait_for_event(&arg);
		switch(event)
		{
			case NETWORK_LAYER_READY:
					get_packet( buffer[next_frame_to_send].data );					//收到一个新的packet 
					//lprintf("Send DATA %d\n",sizeof(buffer[next_frame_to_send].data)/ sizeof(buffer[next_frame_to_send].data[0]) );
					nbuffered = nbuffered + 1 ;	
					send_data(next_frame_to_send, frame_expected,&buffer);
					inc(next_frame_to_send);						//发送者窗口前移（上限前移）
				break;
			case PHYSICAL_LAYER_READY:
				phylsical_ready = 1;
				break;
			case FRAME_RECEIVED:
					 len = recv_frame((unsigned char *)&f, sizeof f);			//从物理层取数据
					 if (len < 5 || (crc32((unsigned char *)&f, len) != 0)) 
						 {
							dbg_event("**** Receiver Error, Bad CRC Checksum\n");
							break;
						 }
					 if (f.kind == FRAME_DATA)
							{
								dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short *)f.data);
								//lprintf("Recv DATA %d\n",sizeof(f.data) / sizeof(f.data[0]));
								if(f.seq == frame_expected)				//收到了期待的帧
									{			
										put_packet(f.data, len-7);		//上交数据给网络层
										inc(frame_expected);			//接收方窗口前移（下限前移）
									}
							}
							while(between(ack_expected, f.ack, next_frame_to_send))			//处理捎带确认
							{
									nbuffered = nbuffered - 1 ;
									stop_timer(ack_expected);				//收到帧，计时器停止
									inc(ack_expected);					//缩小发送者窗口
							}					
				break;
			case DATA_TIMEOUT:
				dbg_event("---- DATA %d timeout\n", arg) ; 
				next_frame_to_send = ack_expected ;								//开始重传
					for( i=1 ; i <= nbuffered ; i++ )
					{
						send_data(next_frame_to_send, frame_expected,&buffer);			//重传
						inc(next_frame_to_send);						//准备下一个要传的dd帧
					}
				break;
		}
		if( ( nbuffered < MAX_SEQ ) && phylsical_ready  )
			enable_network_layer();
		else
			disable_network_layer();
	}
}