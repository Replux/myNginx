#ifndef __NGX_COMM_H__
#define __NGX_COMM_H__


/* * * * * * * * * * * * * * * * *
 *       收发数据包相关的信息
 * * * * * * * * * * * * * * * * */

#define PKG_MAX_LENGTH    30000  //每个包的最大长度【包头+包体】，为了留出一些空间，实际编码是，最大长度必须小于这个值-1000，即29000

#define PKG_HD_BUF       10  //用于收包头的数组的长度，该值要 >sizeof(COMM_PKG_HEADER)

//__________收包的4种状态__________
#define _PKG_HD_INIT         0  //初始状态，准备接收数据包头
#define _PKG_HD_RECVING      1  //接收包头中，包头不完整，继续接收中
#define _PKG_BD_INIT         2  //包头刚好收完，准备接收包体
#define _PKG_BD_RECVING      3  //接收包体中，包体不完整，继续接收中，处理后直接回到_PKG_HD_INIT状态


#pragma pack (1) //对齐方式,1字节对齐【结构之间成员不做任何字节对齐：紧密的排列在一起】

//包头8字节
typedef struct _COMM_PKG_HEADER
{
	unsigned short pkgLen;    //记录报文总长度【包头+包体】，定义PKG_MAX_LENGTH 30000，所以用pkgLen足够保存下
	unsigned short msgCode;   //消息类型，用于区别每个不同类型的消息
	int            crc32;     //CRC32效验，为了防止收发数据中出现收到内容和发送内容不一致的情况，引入这个字段做一个简单的校验用	
}COMM_PKG_HEADER,*LPCOMM_PKG_HEADER;

#pragma pack() //取消指定对齐，恢复缺省对齐


#endif
