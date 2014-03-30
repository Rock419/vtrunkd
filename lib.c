/*  
   vtrunkd - Virtual Tunnel Trunking over TCP/IP network. 

   Copyright (C) 2011  Andrew Gryaznov <realgrandrew@gmail.com>

   Vtrunkd has been derived from VTUN package by Maxim Krasnyansky. 
   vtun Copyright (C) 1998-2000  Maxim Krasnyansky <max_mk@yahoo.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
 */

/*
 * lib.c,v 1.1.1.2.2.9.2.1 2006/11/16 04:03:17 mtbishop Exp
 */ 

#include "config.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <syslog.h>
#include <errno.h>
#include <math.h>

#include "vtun.h"
#include "linkfd.h"
#include "lib.h"

volatile sig_atomic_t __io_canceled = 0;

#ifndef HAVE_SETPROC_TITLE
/* Functions to manipulate with program title */

extern char **environ;
char	*title_start;	/* start of the proc title space */
char	*title_end;     /* end of the proc title space */
int	title_size;

void init_title(int argc,char *argv[], char *envp[], char *name)
{
	int i;

	/*
	 *  Move the environment so settitle can use the space at
	 *  the top of memory.
	 */

	for (i = 0; envp[i]; i++);

	environ = (char **) malloc(sizeof (char *) * (i + 1));

	for(i = 0; envp[i]; i++)
	   environ[i] = strdup(envp[i]);
	environ[i] = NULL;

	/*
	 *  Save start and extent of argv for set_title.
	 */

	title_start = argv[0];

	/*
	 *  Determine how much space we can use for set_title.  
	 *  Use all contiguous argv and envp pointers starting at argv[0]
 	 */
	for(i=0; i<argc; i++)
	    if( !i || title_end == argv[i])
	       title_end = argv[i] + strlen(argv[i]) + 1;

	for(i=0; envp[i]; i++)
  	    if( title_end == envp[i] )
	       title_end = envp[i] + strlen(envp[i]) + 1;
	
	strcpy(title_start, name);
	title_start += strlen(name);
	title_size = title_end - title_start;
}

void set_title(const char *fmt, ...)
{
	char buf[255];
	va_list ap;

	memset(title_start,0,title_size);

	/* print the argument string */
	va_start(ap, fmt);
	vsprintf(buf, fmt, ap);
	va_end(ap);

	if( strlen(buf) > title_size - 1)
	   buf[title_size - 1] = '\0';

	strcat(title_start, buf);
}
#endif  /* HAVE_SETPROC_TITLE */

/* 
 * Print padded messages.
 * Used by 'auth' function to force all messages 
 * to be the same len.
 */
int print_p(int fd,const char *fmt, ...)
{
	char buf[VTUN_MESG_SIZE];
	va_list ap;

	memset(buf,0,sizeof(buf));

	/* print the argument string */
	va_start(ap, fmt);
	vsnprintf(buf,sizeof(buf)-1, fmt, ap);
	va_end(ap);
  
	return write_n(fd, buf, sizeof(buf));
}

/* Read N bytes with timeout */
int readn_t(int fd, void *buf, size_t count, time_t timeout) 
{
	fd_set fdset;
	struct timeval tv;

	tv.tv_usec=0; tv.tv_sec=timeout;

	FD_ZERO(&fdset);
	FD_SET(fd,&fdset);
	if( select(fd+1,&fdset,NULL,NULL,&tv) <= 0)
	   return -1;

	return read_n(fd, buf, count);
}

/* 
 * Substitutes opt in place off '%X'. 
 * Returns new string.
 */
char * subst_opt(char *str, struct vtun_sopt *opt)
{
    register int slen, olen, sp, np;
    register char *optr, *nstr, *tmp;
    char buf[10];

    if( !str ) return NULL;

    slen = strlen(str) + 1;
    if( !(nstr = malloc(slen)) )
       return str;

    sp = np = 0;
    while( str[sp] ){
       switch( str[sp] ){
          case '%':
             optr = NULL;
             /* Check supported opt */
             switch( str[sp+1] ){
                case '%':
                case 'd':
                   optr=opt->dev;
                   break;
                case 'A':
                   optr=opt->laddr;
                   break;
                case 'P':
		   sprintf(buf,"%d",opt->lport);
                   optr=buf;
                   break;
                case 'a':
                   optr=opt->raddr;
                   break;
                case 'p':
		   sprintf(buf,"%d",opt->rport);
                   optr=buf;
                   break;
                default:
                   sp++;
                   continue;
             }
             if( optr ){
                /* Opt found substitute */
                olen = strlen(optr);
                slen = slen - 2 + olen;
                if( !(tmp = realloc(nstr, slen)) ){
                   free(nstr);
                   return str;
                }
                nstr = tmp;
                memcpy(nstr + np, optr, olen);
                np += olen;
             }
             sp += 2;
             continue;

          case '\\':
             nstr[np++] = str[sp++];
             if( !nstr[sp] )
                continue;
             /* fall through */
          default:
             nstr[np++] = str[sp++];
             break;
       }
    }
    nstr[np] = '\0';
    return nstr;
}

/* 
 * Split arguments string.
 * ' ' - group arguments
 * Modifies original string. 
 */
void split_args(char *str, char **argv)
{       
     register int i = 0;
     int mode = 0;

     while( str && *str ){
        switch( *str ){
           case ' ':
              if( mode == 1 ){
                 *str = '\0';
                 mode = 0;
                 i++;
              }
              break;

           case '\'':
              if( !mode ){
                 argv[i] = str+1;
                 mode = 2;
              } else {
                 memmove(argv[i]+1, argv[i], str - argv[i]);
                 argv[i]++;

                 if( mode == 1 )
                    mode = 2;
                 else
                    mode = 1;
              }
              break;

           case '\\':
              if( mode ){
                 memmove(argv[i]+1, argv[i], str - argv[i]);
                 argv[i]++;
              }
	      if( !*(++str) ) continue;
	      /*Fall through */

           default:
              if( !mode ){
                 argv[i] = str;
                 mode = 1;
              }
              break;
        }
        str++;
     }
     if( mode == 1 || mode == 2)
	i++;

     argv[i]=NULL;
}
 
int run_cmd(void *d, void *opt)
{
     struct vtun_cmd *cmd = d;	
     char *argv[50], *args;
     int pid, st;

     switch( (pid=fork()) ){
	case 0:
	   break;
	case -1:
	   vtun_syslog(LOG_ERR,"Couldn't fork()");
	   return 0;
	default:
    	   if( cmd->flags & VTUN_CMD_WAIT ){
	      /* Wait for termination */
	      if( waitpid(pid,&st,0) > 0 && (WIFEXITED(st) && WEXITSTATUS(st)) )
		 vtun_syslog(LOG_INFO,"Command [%s %.20s] error %d", 
				cmd->prog ? cmd->prog : "sh",
				cmd->args ? cmd->args : "", 
				WEXITSTATUS(st) );
	   }
    	   if( cmd->flags & VTUN_CMD_DELAY ){
	      struct timespec tm = { VTUN_DELAY_SEC, 0 };
	      /* Small delay hack to sleep after pppd start.
	       * Until I have no good solution for solving 
	       * PPP + route problem  */
	      nanosleep(&tm, NULL);
	   }
	   return 0;	 
     }

     args = subst_opt(cmd->args, opt);
     if( !cmd->prog ){
	/* Run using shell */
	cmd->prog = "/bin/sh";
        argv[0] = "sh";	
	argv[1] = "-c";
	argv[2] = args;
	argv[3] = NULL;
     } else {
        argv[0] = cmd->prog;	
        split_args(args, argv + 1);
     }
     execv(cmd->prog, argv);

     vtun_syslog(LOG_ERR,"Couldn't exec program %s", cmd->prog);
     exit(1);
}

void free_sopt( struct vtun_sopt *opt )
{
     if( opt->dev ){
	free(opt->dev);
        opt->dev = NULL;
     }

     if( opt->laddr ){
	free(opt->laddr);
        opt->laddr = NULL;
     }

     if( opt->raddr ){
	free(opt->raddr);
        opt->raddr = NULL;
     }
}

static int llsqrt(long a)
{
        long long prev = ~((long long)1 << 63);
        long long x = a;

        if (x > 0) {
                while (x < prev) {
                        prev = x;
                        x = (x+(a/x))/2;
                }
        }

        return (int)x;
}

/*
 * finds the standard deviation
 * =sqrt(sum)x-mean(x)^2)/n)
 */
int std_dev(int nums[], int len)
{
	long sum = 0;
	long mean = 0;
	if(len==0) return 0;
	long llen = len;
	
	int i;
	
	for(i=0;i<len;i++) {
		sum+=nums[i];
	}
	mean = sum/llen;
	sum = 0;
	for(i = 0; i < len; i++)
		sum += abs(nums[i]-mean); // for stddev need to mult
	
	//return llsqrt(sum/len);
	return (sum/len);
}

void vtun_syslog (int priority, char *format, ...)
{
   static volatile sig_atomic_t in_syslog= 0;
   char buf[512];
   va_list ap;

   if(! in_syslog) {
      in_syslog = 1;
    
      va_start(ap, format);
      vsnprintf(buf, sizeof(buf)-1, format, ap);
      syslog(priority, "%s", buf);
      va_end(ap);

      in_syslog = 0;
   }
}

/* Methods for periodic JSON logs */

int start_json(char *buf, int *pos) {
    int bs=0;
    memset(buf, 0, JS_MAX);
    *pos = 0;

    bs = sprintf(buf, "{");
    *pos = *pos + bs;
    return 0;
}

int add_json(char *buf, int *pos, const char *name, const char *format, ...) {
    va_list args;
    int bs = 0;
    if (*pos > (JS_MAX/2)) return -1;
    bs = sprintf(buf + *pos, "\"%s\":", name);
    *pos = *pos + bs;
    
    va_start(args, format);
    bs = vsprintf(buf+*pos, format, args);
    va_end(args);
    
    *pos = *pos + bs;

    bs = sprintf(buf + *pos, ",");
    *pos = *pos + bs;
    return bs;
}

int print_json(char *buf, int *pos) {
    buf[*pos-1] = 0;
    vtun_syslog(LOG_INFO, "%s}", buf);
    return 0;
}


/* Methods for fast changing variable logging */
int start_json_arr(char *buf, int *pos, const char *name) {
    int bs=0;
    memset(buf, 0, JS_MAX);
    //struct timeval dt;
    //gettimeofday(&dt, NULL);
    *pos = 0;

    //bs = sprintf(buf, "%ld.%06ld: {", dt.tv_sec, dt.tv_usec);
    bs = sprintf(buf, "{\"%s\": [", name); // no need for TS in slow-ticking jsons

    *pos = *pos + bs;
    return 0;
}

int add_json_arr(char *buf, int *pos, const char *format, ...) {
    va_list args;
    int bs = 0;
    if (*pos > (JS_MAX/2)) return -1;

    va_start(args, format);
    bs = vsprintf(buf+*pos, format, args);
    va_end(args);
    
    *pos = *pos + bs;

    bs = sprintf(buf + *pos, ",");
    *pos = *pos + bs;
    return bs;
}

int print_json_arr(char *buf, int *pos) {
    buf[*pos-1] = 0;
    vtun_syslog(LOG_INFO, "%s]}", buf);
    return 0;
}



#ifdef TIMEWARP
int print_tw(char *buf, int *pos, const char *format, ...) {
    va_list args;
    int slen;
    struct timeval dt;
    gettimeofday(&dt, NULL);
    
    sprintf(buf + *pos, "\n%ld.%06ld:    ", dt.tv_sec, dt.tv_usec);
    *pos = *pos + 20;
    
    va_start(args, format);
    int out = vsprintf(buf+*pos, format, args);
    va_end(args);
    
    slen = strlen(buf+*pos);
    *pos = *pos + slen;
    if(*pos > TW_MAX - 10000) { // WARNING: 10000 max per line!
        sprintf(buf + *pos, "---- Overflow!\n");
        *pos = 0;
    }
    return out;
}

int flush_tw(char *buf, int *tw_cur) {
    // flush, memset
    int fd = open("/tmp/TIMEWARP.log", O_WRONLY | O_APPEND);
    int slen = strlen(buf);
    //vtun_syslog(LOG_INFO, "FLUSH! %d", slen);
    int len = write(fd, buf, slen);
    close(fd);
    memset(buf, 0, TW_MAX);
    *tw_cur = 0;
    return len;
}

int start_tw(char *buf, int *c) {
    memset(buf, 0, TW_MAX);
    *c = 0;
    return 0;
}
#endif

