/*
 **************************************************************************
 * @file    socket.c
 * @author  Liwa Lin
 * @version V1.0.1
 * @date    Sep 8, 2016
 * @version V1.0.0
 * @date    Sep 1, 2016
 **************************************************************************
 * @attetion  Based on GSM Module SIM7600C
 *
 * Copyright(c) 2016 QXWZ Corporation.  All rights reserved.
 */

#include "socket.h"
#include "delay.h"
#include "usart.h"
#include "main.h"
#include "prefs.h"
#include "ef_qx.h"
#include "sim7600.h"
#include "wt5883.h"

#undef QXLOG
#define QXLOG printf


typedef  uint8_t  BOOL;
#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

//GSM回复ERROR
#define GSM_ACK_ERROR             1
//GSM重启
#define GSM_ACK_IIII              2
//GSM TCP连接失败
#define GSM_ACK_CONNECT_FAIL      3
//GSM 远端关闭连接
#define GSM_ACK_CLOSED            4
//等待GSM模块回复超时
#define GSM_ACK_TIMEOUT           5
//已建立连接
#define GSM_ACK_ALREAY_CONNECT    6
//模块不允许操作
#define GSM_ACK_CME_ERROR         7

//socket 默认通道号
#define SOC_DEFUALT_CHANNEL       1   
//串口数据接收缓存区
#define BUF_MAX                   40 
//AT指令等待回复超时时间，单位秒
#define AT_CMD_TIMEOUT    		   	5

#define RECV_DATA_BUF_MAX         2048

#define WAIT_CFG_TRY_SECOND       20
//统计使用
uint8_t recv_IIII_cnt = 0;
uint8_t recv_CLOSED_cnt = 0;
uint32_t timer_cnt = 0;
int32_t uart_irq_time_need = 0;


/** ----------------------------------------- INTERNAL ------------------------------------------*/

//是否已经初始化GSM模块
static BOOL s_is_init = FALSE;
//重启模块标志
static BOOL s_restart_gsm_flag = FALSE;

//开始接收GPRS TCP应用层数据
static BOOL s_begin_recive = FALSE;

static uint8_t s_first_index = 0;
static uint16_t s_body_len = 0;
//已经接收到的网络应用层的数据长度
static uint16_t s_has_recived_len = 0;

//static char body_len_buf[5] = {0};//缓存body长度
static char uart_exception_buf[10] = {0};
static char uart_buf[BUF_MAX] = {0};
static char recived_buf[RECV_DATA_BUF_MAX] = {0};//接收数据的缓存


//定时器相关
static BOOL s_timer_start_flag = FALSE;	
static BOOL s_at_ack_timeout_flag = FALSE; 
static uint8_t s_at_ack_wait_time = 0;
static uint16_t s_tick_cnt = 0;

//qxwz_sdk 状态
static uint32_t qxwz_sdk_ntrip_status = 0;

static int8_t qxwz_soc_init(void);
static int8_t send_at_command(char *b,char *a,uint8_t wait_time); 
static uint8_t find_string(char *str);
static void clear_buf(void);

static int8_t set_ate0(void);
static int8_t init_gsm_gprs(void);
static void get_gsm_status(void);
static uint8_t gsm_ack_exception(void);
static void start_at_ack_timer(uint8_t second);


//socket发生错误的情况下调用

static void query_uart_buf(uint8_t recv);
static void uart_recv_exception(uint8_t recv);

//异步socket
static unsigned char  s_connect_ok_flag = FALSE;
static unsigned char  s_connect_fail_flag = FALSE;

static unsigned char s_send_ok_flag = FALSE;
static unsigned char  s_send_fail_flag = FALSE;
static unsigned char s_close_ok_flag = FALSE;
static unsigned char s_close_fail_flag = FALSE;


/**-------------------------模拟socket create connect send  close---------------------------------*/
/*******************************************************************************
函 数 名：void qxwz_soc_create
功能描述：网络服务器初始化
入口参数：soc:通道 address:服务器地址
返回参数：0 成功
创建时间: 2017-11-02 by zam
********************************************************************************/
qxwz_soc qxwz_soc_create(void)
{
	qxwz_soc  socket_fd = -1;
	
	socket_fd = qxwz_soc_init();
	if(socket_fd >= 0)
	{
		QXLOG("socket connect successful=%d \r\n",socket_fd);
		return socket_fd;
	}
	else
	{
		QXLOG(" socket connect erro\r\n");
		return -1;
	}
}

/*******************************************************************************
函 数 名：void qxwz_soc_connect
功能描述：连接网络服务器
入口参数：soc:通道 address:服务器地址
返回参数：0 成功
创建时间: 2017-11-02 by zam
********************************************************************************/
int8_t qxwz_soc_connect(qxwz_soc soc,qxwz_soc_address address)
{
	int8_t ret = 0;
	char connect_buf[200];
	memset(connect_buf,0,sizeof(connect_buf));
	sprintf(connect_buf,"AT+CIPOPEN=0,\"TCP\",\"%s\",%d",address.hostName,address.port);
	QXLOG("connect servece= %s\r\n",connect_buf);
	if((ret = send_at_command(connect_buf,"OPEN",3*AT_CMD_TIMEOUT)) != 0)
	{
		QXLOG("connect erro=%d \r\n",ret);
		if(qxwz_prefs_flags_get() & QXWZ_PREFS_FLAG_SOCKET_ASYN)
		{
			s_connect_fail_flag = TRUE;
		}
		return ret;
	}
	return 0;
}
/*******************************************************************************
函 数 名：void qxwz_soc_send
功能描述：关闭网络模块发送数据
入口参数：soc:通道 send_buffer:待发送数据 length:数据长度
返回参数：数据发送长度
创建时间: 2017-11-02 by zam
********************************************************************************/
qxwz_ssize_t qxwz_soc_send(qxwz_soc soc,char *send_buffer,size_t length)
{
	int8_t ret = 0;
	char sendbuf[1024];
	memset(sendbuf,0,sizeof(sendbuf));
	ret = send_at_command("AT+CIPSEND=0,",">",3*AT_CMD_TIMEOUT);
	if(ret != 0)
	{
		QXLOG("can not connect socker!\r\n");//发送数据
		qxwz_soc_error(SOC_DEFUALT_CHANNEL);
		return -1;	
	}
	QXLOG("send date......\r\n");//发送数据
	sprintf(sendbuf,"%s\32\0",send_buffer);
	QXLOG("%s\32\0",send_buffer);
	ret = send_at_command(sendbuf,"OK",3*AT_CMD_TIMEOUT);  //回复OK，时间可以调
	if(ret != 0)
	{
		QXLOG("send erro.....\r\n");//发送数据
		if(qxwz_prefs_flags_get() & QXWZ_PREFS_FLAG_SOCKET_ASYN)
		{
			s_send_fail_flag = TRUE;
		}
		qxwz_soc_error(SOC_DEFUALT_CHANNEL);
		return -1;
	}
	return length;
}

/*******************************************************************************
函 数 名：void qxwz_soc_close
功能描述：关闭网络模块数据
入口参数：
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
void qxwz_soc_close(qxwz_soc soc)
{
	uint8_t local_soc = soc;
	int8_t ret = 0;	
	QXLOG("select close socket=%d.....\r\n",local_soc);//主动关闭连接
	ret = send_at_command("AT+CIPCLOSE=0","CLOSE",3*AT_CMD_TIMEOUT);	//关闭连接
	if(ret != 0)
	{
		QXLOG("close socket erro.....\r\n");//发送数据
		if(qxwz_prefs_flags_get() & QXWZ_PREFS_FLAG_SOCKET_ASYN)
		{
			s_close_fail_flag = TRUE;
		}
	}
}

/*******************************************************************************
函 数 名：void qxwz_soc_init
功能描述：网络模块初始化
入口参数：
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
qxwz_soc qxwz_soc_init(void)
{
	int8_t ret = 0;
	if(Init_Sim)
	{
		return 1;
	}
	if(s_is_init == FALSE)
	{
		ret = init_gsm_gprs();
		if(ret == 0)
		{
			s_is_init = TRUE;
		}else
		{
			QXLOG("init model erro code=%d\r\n",ret);
			return -1;
		}
	}
	//GSM单通道模式默认使用通道1
	return 1;	
	
}
/**------------------------------------------中断服务程序相关--------------------------------------------*/
/*******************************************************************************
函 数 名：void USART1_IRQHandler
功能描述：�2中断服务程序,接收GSM模块应答的数据
入口参数：
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
void USART1_IRQHandler(void)                	
{
	uint8_t res = 0;
	OSIntEnter();
	if(USART_GetITStatus(GSM_USART, USART_IT_RXNE) != RESET) 
	{
		res = USART_ReceiveData(GSM_USART);
		uart_recv_exception(res);
		if(s_begin_recive == TRUE)
		{
			recived_buf[s_has_recived_len] = res;
			s_has_recived_len++;
			if(s_has_recived_len >= s_body_len)
			{
				s_begin_recive = FALSE;
				recived_buf[s_has_recived_len+1] = 0;
				qxwz_on_soc_data_received(SOC_DEFUALT_CHANNEL,recived_buf,s_has_recived_len);
			}
		}
		else
		{
			uart_buf[s_first_index] = res;  	  //将接收到的字符串存到缓存中 
			if(!Task_Flag.Flag_Bit.Debug_Date)
			{
				DEBUG_UART->DR = res;//将接收到的数据转发到调试串口
			}
			else
			{
				BLUETOOTH_USART->DR = res;//将接收到的数据转发到调试串口
			}
			s_first_index++;                		//缓存指针向后移动
			if(s_first_index >= (BUF_MAX-1))
			{
				s_first_index = 0;
			}
			query_uart_buf(res);
		}
	}
	OSIntExit();  
}

/*******************************************************************************
函 数 名：void uart_recv_exception
功能描述：查询串口命令接收缓存区
入口参数：@param  recv    串口接收到的字符
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
static void query_uart_buf(uint8_t recv)
{
	char *qx_cnt;
	if((recv =='\n'))//用来判断是否有数据返回  gsm模块返回的数据
	{
		if((qx_cnt=strstr(uart_buf,"+IPD"))!=NULL)
		{
			s_begin_recive = 1;
			qx_cnt+=4;
			s_body_len =atoi(qx_cnt);//计算将要收到的数据
			if(s_body_len > sizeof(recived_buf)-1)
			{
				s_body_len	= sizeof(recived_buf)-1;
			}
			s_has_recived_len = 0;
			clear_buf();
		}
		else if((qx_cnt=strstr(uart_buf,"SIM not"))!=NULL)
		{
			Play_voice(NO_SIM);
			QXLOG("$NO SIM\r\n");//将其他数据发回到底板去
			clear_buf();
		}
		else if((qx_cnt=strstr(uart_buf,"ERROR"))!=NULL)
		{
			clear_buf();
		}
	}
	if(qxwz_prefs_flags_get() & QXWZ_PREFS_FLAG_SOCKET_ASYN)//使能异步SOCKET，才需要检测完成情况
	{
		if(recv == 'K')//判断一些带有OK的正常状态
		{
			if((strcmp(uart_buf,"CLOSE"))!=NULL)
			{
				s_close_ok_flag = TRUE;
			}
			else if((strcmp(uart_buf,"CONNECT"))!=NULL)
			{
				s_connect_ok_flag = TRUE;
			}
			else if((strcmp(uart_buf,"SEND"))!=NULL)
			{
				s_send_ok_flag = TRUE;
			}
		}	
	}
	
}
/*******************************************************************************
函 数 名：void uart_recv_exception
功能描述：判断接收错误数据
入口参数：
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
void uart_recv_exception(uint8_t recv)
{
	static BOOL s_begin_exception_recv_flag = FALSE;
	static uint8_t s_index = 0;
	if(!s_begin_exception_recv_flag)
	{
		if((recv == 'I')||(recv == 'C'))
		{
			s_begin_exception_recv_flag = TRUE;
			s_index = 0;
			memset(uart_exception_buf,0,sizeof(uart_exception_buf));
			uart_exception_buf[s_index++] = recv;//记录异常首字符
		}
	}
	else
	{
		uart_exception_buf[s_index++] = recv;
		if(s_index > sizeof(uart_exception_buf))
		{
			s_index = 0;
			s_begin_exception_recv_flag = FALSE;
		}
		if(!strcmp(uart_exception_buf,"IIII"))
		{
			s_begin_exception_recv_flag = FALSE;//结束接收
			qxwz_soc_error(SOC_DEFUALT_CHANNEL);
			s_is_init = FALSE;//重新初始化
		}
		else if(!strcmp(uart_exception_buf,"CLOSE"))
		{
			s_begin_exception_recv_flag = FALSE;//结束接收
			qxwz_soc_error(SOC_DEFUALT_CHANNEL);
		}
	}
}

/*******************************************************************************
函 数 名：void TIM3_IRQHandler
功能描述：定时器3中断服务程序
入口参数：
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
void TIM3_IRQHandler(void)   
{
	if (TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET) 
	{
		TIM_ClearITPendingBit(TIM3, TIM_IT_Update);  
		timer_tick(TIMER_FEQ);
	}
}

/*******************************************************************************
函 数 名：void timer_tick
功能描述：在定时中断中被调用
入口参数：	@param  tick_feq              1秒内调用频率,单位(HZ)
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
void timer_tick(uint16_t tick_feq)
{
	if(s_timer_start_flag)
	{
		if(s_tick_cnt >= s_at_ack_wait_time*tick_feq)
		{
				s_at_ack_timeout_flag = TRUE;
		}	
		s_tick_cnt++;
	}
	else
	{
		s_tick_cnt = 0;
	}
}

/**-------------------------------------AT指令相关------------------------------------------------*/
/*******************************************************************************
函 数 名：void send_at_command
功能描述：启用定时器 
入口参数：	@param  send_str              待发送的AT指令	 @param  excepted_recv_str     期待接收的应答
返回参数：  @param  wait_time             超时时间,单位(S)
创建时间: 2017-11-02 by zam
********************************************************************************/
static int8_t send_at_command(char *send_str,char *excepted_recv_str,uint8_t wait_time)         
{
	uint8_t ret = 0;									
	if(send_str == NULL)
	{
		return -1;
	}
	clear_buf(); //清串口接收缓存区
	s_timer_start_flag = FALSE;
	for (; *send_str!='\0';send_str++)//发送AT命令
	{
		while(USART_GetFlagStatus(GSM_USART, USART_FLAG_TC) == RESET);
		USART_SendData(GSM_USART,*send_str);
	}
	UART_SendString(GSM_USART,"\r\n");
	if((excepted_recv_str != NULL)&&(wait_time > 0))
	{
		start_at_ack_timer(wait_time);//启动超时控制定时器
	}
	else
	{
		return 0;//直接返回
	}
	while(1)                    
	{
		if(!find_string(excepted_recv_str))
		{
			ret = gsm_ack_exception();	//没收到期待收到的GSM 应答	
			if(ret != 0)
			{
				break;
			}
    	}
		else
    	{
			//find the excepted string
			break;   //跳出循环
		}
		if(s_at_ack_timeout_flag)//timeout
		{
			ret = GSM_ACK_TIMEOUT;
			break;
		}
	}//while(1)
	
	if(ret == GSM_ACK_ALREAY_CONNECT)
	{
		ret = 0;
	}
	clear_buf(); 
	s_timer_start_flag = FALSE;  //停止计时器
	s_at_ack_timeout_flag = FALSE;
	return ret;
}
/*******************************************************************************
函 数 名：void start_at_ack_timer(void)
功能描述： 判断缓存中是否含有指定的字符串
入口参数：	@param  str              待发送的AT指令	
返回参数： @return 1 找到指定字符，0 未找到指定字符 
创建时间: 2017-11-02 by zam
********************************************************************************/
static uint8_t find_string(char *str)
{ 
	if(NULL == str)
	{
		return 0;
	}
  	if(strstr(uart_buf,str) != NULL)
	  return 1;
	else
		return 0;
}
/*******************************************************************************
函 数 名：void start_at_ack_timer(void)
功能描述：启用定时器 
入口参数：		second  			  定时时间,单位(S)		
返回参数： 
创建时间: 2017-11-02 by zam
********************************************************************************/
static void start_at_ack_timer(uint8_t second)
{
	s_at_ack_wait_time = second; 
	s_timer_start_flag = TRUE;     
}
/*******************************************************************************
函 数 名：void clear_buf(void)
功能描述：清除串口接收缓存区,并置接收偏移量为0		
入口参数：							
返回参数： 
创建时间: 2017-11-02 by zam
********************************************************************************/
static void clear_buf(void)
{
	//uint16_t k;
	//for(k = 0;k < BUF_MAX;k++) 
	//{
	//	uart_buf[k] = 0x00;
	//}
	memset(uart_buf, 0, BUF_MAX);
  	s_first_index = 0;              //接收字符串的起始存储位置
}

/*******************************************************************************
函 数 名：void wait_creg(void)
功能描述：等待模块注册成功				
入口参数：							
返回参数： @return 0  ok   else  errro
创建时间: 2017-11-02 by zam
********************************************************************************/
int8_t wait_creg(void)
{
	uint8_t i = 0;
	uint8_t k;
	BOOL first_command = TRUE;
	clear_buf();
	QXLOG("reging.....");
  	while(i == 0)        			
	{
		clear_buf();  
		UART_SendString(GSM_USART,"AT+CREG?");
		UART_SendString(GSM_USART,"\r\n");
		delay_ms(3000);  	
		if(first_command)
		{
			start_at_ack_timer(WAIT_CFG_TRY_SECOND);
			first_command = FALSE;
		}
	  	for(k = 0;k < BUF_MAX;k++) 
		{
			if(uart_buf[k] == ':')
			{
				if((uart_buf[k+4] == '1')||(uart_buf[k+4] == '5'))
				{
					i = 1;
					QXLOG("\r\n");
					return  0;
				}
			}
		}
		if(s_at_ack_timeout_flag)
		{
			s_restart_gsm_flag = TRUE;
			s_timer_start_flag = FALSE;//停止计时器
			s_at_ack_timeout_flag = FALSE;
			return  GSM_ACK_TIMEOUT;
		}
	}//while(i == 0)   
	return 0;
}
	
/*******************************************************************************
函 数 名：void set_ate0(void)
功能描述：关掉回显					
入口参数：							
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
static int8_t set_ate0(void)
{
	return (send_at_command("ATE0","OK",AT_CMD_TIMEOUT));								//取消回显		
}
/*******************************************************************************
函 数 名：int8_t init_gsm_gprs(void)
功能描述：初始化tcp网络					
入口参数：							
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
static int8_t init_gsm_gprs(void)
{
	int8_t ret = 0;
	delay_ms(3000);  
	ret = send_at_command("AT+CICCID","SIM not",3);
	ret = wait_creg();
	if(ret == 0)
	{
		QXLOG("reg successful.....\r\n");
	}
	else
	{
		QXLOG("reg erro.....\r\n");
		Play_voice(NET_ERRO);//注册错误
		return -1;
	}
	if((ret = set_ate0()) != 0)//关闭回显
	{
		return ret;
	}
//	send_at_command("AT+CREG=1","OK",3*AT_CMD_TIMEOUT);//开启显示网络注册等信息
	if((ret = send_at_command("AT+CGREG=1","OK",3*AT_CMD_TIMEOUT)) != 0) //设置GPRS移动台类别为A,支持包交换和数据交换 
	{
		QXLOG("net reg erro!\r\n");
		return ret;
	}		
//	if((ret = send_at_command("AT+CGCLASS=\"A\"","OK",3*AT_CMD_TIMEOUT)) != 0) //设置GPRS移动台类别为A,支持包交换和数据交换 
//	{
//		QXLOG("A类移动台设置错误!\r\n");
//		return ret;
//	}		
//	if((ret = send_at_command("AT+CGATT=1","OK",3*AT_CMD_TIMEOUT)) != 0)//附着GPRS业务 
//	{
//		QXLOG("GPRS数据错误!\r\n");
//		return ret;
//	}
//	if((ret =send_at_command("AT+CGACT=1","OK",AT_CMD_TIMEOUT)) != 0)
//	{
//		QXLOG("PDP未激活!\r\n");
//		return ret;
//	}
	if((ret =send_at_command("AT+CGDCONT=1,\"IP\",\"CMNET\"","OK",3*AT_CMD_TIMEOUT)) != 0)
	{
		//设置PDP上下文,互联网接协议,接入点等信息
		QXLOG("set ip erro!\r\n");
		return ret;
	}
	if((ret = send_at_command("AT+CSOCKSETPN=1","OK",3*AT_CMD_TIMEOUT)) != 0) 
	{
		QXLOG("set CSOCKSETPN erro!\r\n");
		return ret;
	}
	if((ret = send_at_command("AT+CIPMODE=0","OK",3*AT_CMD_TIMEOUT)) != 0) 
	{
		//附着GPRS业务
		QXLOG("set model erro!\r\n");
		return ret;
	}
	if((ret = send_at_command("AT+NETOPEN","OK",3*AT_CMD_TIMEOUT)) != 0) 
	{
		QXLOG("net can not open!\r\n");
		return ret;
	}
	if((ret = send_at_command("AT+CIPHEAD=1","OK",2*AT_CMD_TIMEOUT)) != 0) 
	{
		//设置接收数据显示IP头(方便判断数据来源,仅在单路连接有效)
		QXLOG("set ip head erro!\r\n");
		return ret;
	}
	return ret;
}
/*******************************************************************************
函 数 名：int8_t gsm_ack_exception(void)
功能描述：异常数据接收				
入口参数：							
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
static uint8_t gsm_ack_exception(void)
{
	uint8_t ret = 0;
	if(find_string("+CME ERROR"))
	{
		if(qxwz_sdk_ntrip_status == QXWZ_STATUS_NTRIP_CONNECTED)
		{
			//说明远端服务器已断开
			qxwz_soc_error(SOC_DEFUALT_CHANNEL);
		}
		ret = GSM_ACK_CME_ERROR;//GSM:操作不允许
	}
	else if(find_string("CONNECT FAIL"))
	{
		s_restart_gsm_flag = TRUE;
		s_connect_fail_flag = TRUE;
		ret = GSM_ACK_CONNECT_FAIL;
	}
	else if(find_string("IIII"))
	{
		ret = GSM_ACK_IIII;//模块自己重启
	}
	else if(find_string("ALREAY CONNECT"))
	{
		ret = GSM_ACK_ALREAY_CONNECT;//已建立连接
	}
	else if(find_string("ERROR"))
	{//指令调用错误
		ret = GSM_ACK_ERROR;  //error，指令格式错误
	}
	if(ret != 0)
	{
		QXLOG("GSM init erro=%d....\r\n",ret);
	}
	return ret;
}

/*******************************************************************************
函 数 名：int8_t get_gsm_status(void)
功能描述：获取模块工作状态		
入口参数：							
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
static void get_gsm_status(void)
{
	send_at_command("AT+CIPSTATUS","OK",AT_CMD_TIMEOUT);	//获取连接状态
}
/*******************************************************************************
函 数 名：void enable_gsm_report_error(void)
功能描述：使能模块上报错误信息	
入口参数：							
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
void enable_gsm_report_error(void)
{
	send_at_command("AT+CMEE=?","OK",AT_CMD_TIMEOUT);	
	send_at_command("AT+CMEE=2","OK",AT_CMD_TIMEOUT);	
}

/*******************************************************************************
函 数 名：void restart_connect(void)
功能描述：GSM重启,在led_task调用,轮询
入口参数：							
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
void restart_connect(void)
{
	if(s_restart_gsm_flag)
	{
		//send_at_command("AT+CFUN=0","OK",2*AT_CMD_TIMEOUT);	//进入最小功能模式
		//delay_ms(1000);
		//send_at_command("AT+CFUN=1","OK",3*AT_CMD_TIMEOUT);	//进入全面功能模式
		s_restart_gsm_flag = FALSE;
		s_is_init = FALSE;
	}
}
/*******************************************************************************
函 数 名：void get_qxwz_sdk_status(void)
功能描述：获取SDK状态
入口参数：							
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
void get_qxwz_sdk_status(qxwz_rtcm_status_code status)
{
	qxwz_sdk_ntrip_status = status;
}

/**-------------------------------------异步socket list通知机制---------------------------------------*/
/*******************************************************************************
函 数 名：void async_notify(void)
功能描述：异步socket list通知机制
入口参数：							
返回参数：
创建时间: 2017-11-02 by zam
********************************************************************************/
void async_notify(void)
{
	if(qxwz_prefs_flags_get() & QXWZ_PREFS_FLAG_SOCKET_ASYN)
	{
		//connect
		if(s_connect_ok_flag)
		{
			qxwz_soc_connect_complete(SOC_DEFUALT_CHANNEL,0);
			s_connect_ok_flag = FALSE;
		}
		else if(s_connect_fail_flag)
		{
			qxwz_soc_connect_complete(SOC_DEFUALT_CHANNEL,2);
			s_connect_fail_flag = FALSE;
		}
		//send
		if(s_send_ok_flag)
		{
			qxwz_soc_send_complete(SOC_DEFUALT_CHANNEL,0);
			s_send_ok_flag = FALSE;
		}
		else if(s_send_fail_flag)
		{
			qxwz_soc_send_complete(SOC_DEFUALT_CHANNEL,-1);
			s_send_fail_flag = FALSE;
		}
		//close
		if(s_close_ok_flag)
		{
			qxwz_soc_close_complete(SOC_DEFUALT_CHANNEL,0);
			s_close_ok_flag = FALSE;
		}
		else if(s_close_fail_flag)
		{
			qxwz_soc_close_complete(SOC_DEFUALT_CHANNEL,2);
			s_close_fail_flag = FALSE;
		}
	}
	
}

