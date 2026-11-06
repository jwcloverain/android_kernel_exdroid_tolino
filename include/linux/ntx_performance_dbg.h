#ifndef _ntx_performance_dbg_h //[
#define _ntx_performance_dbg_h

#include <linux/ktime.h>

#if defined(NTX_PERFORMANCE_DBG_EN) //[

#define PTIME_DECLARE(ptime_name) \
	ktime_t gtKtime##ptime_name

#define PTIME_DECLARE_EXTERN(ptime_name) \
	extern ktime_t gtKtime##ptime_name

#define PTIME_GET_CURTIME(ptime_name)	\
	gtKtime##ptime_name=ktime_get()

#define PTIME_SHOW_DIFF(ptime_name,usecs_to_show,fmt, ...) \
{\
	ktime_t calltime;\
	u64 usecs64;\
	int usecs;\
	calltime = ktime_get();\
	usecs64 = ktime_to_ns(ktime_sub(calltime, gtKtime##ptime_name));\
	do_div(usecs64, NSEC_PER_USEC);\
	usecs = usecs64;\
	if (usecs == 0)\
		usecs = 1;\
	if( usecs > usecs_to_show) {\
		printk(fmt, ##__VA_ARGS__);\
		printk(" takes %ld.%03ld msecs\n",\
			usecs / USEC_PER_MSEC, usecs % USEC_PER_MSEC);\
	}\
}
/**
 * ktime_compare - Compares two ktime_t variables for less, greater or equal
 * @cmp1:	comparable1
 * @cmp2:	comparable2
 *
 * Return: ...
 *   cmp1  < cmp2: return <0
 *   cmp1 == cmp2: return 0
 *   cmp1  > cmp2: return >0
 */
#define PTIME_COMPARE(ptime1,ptime2)	\
	ktime_compare(gtKtime##ptime1,gtKtime##ptime2)

#else //][!

#define PTIME_DECLARE(ptime_name) 
#define PTIME_DECLARE_EXTERN(ptime_name) 
#define PTIME_GET_CURTIME(ptime_name)
#define PTIME_SHOW_DIFF(ptime_name,usecs_to_show,fmt, ...) 
#define PTIME_COMPARE(ptime1,ptime2)	

#endif //] NTX_PERFORMANCE_DBG_EN
#endif //] _ntx_performance_dbg_h

