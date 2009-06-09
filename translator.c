/*
 * =====================================================================================
 *
 *       Filename:  translator.c
 *
 *    Description:  a translator for omimic (from input events to usb usages)
 *
 *        Version:  1.0
 *        Created:  03/18/2009 04:31:32 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  l_amee (l_amee), l04m33@gmail.com
 *        Company:  SYSU
 *
 * =====================================================================================
 */

#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>


__u8 key_map[246] = {
    0x00, // reserved
    0x29, // escape
    0x1e, // 1
    0x1f, // 2
    0x20, // 3
    0x21, // 4
    0x22, // 5
    0x23, // 6
    0x24, // 7
    0x25, // 8
    0x26, // 9
    0x27, // 0
    0x2d, // -
    0x2e, // =
    0x2a, // backspace
    0x2b, // tab
    0x14, // q
    0x1a, // w
    0x08, // e
    0x15, // r
    0x17, // t
    0x1c, // y
    0x18, // u
    0x0c, // i
    0x12, // o
    0x13, // p
    0x2f, // {
    0x30, // }
    0x28, // enter
    0x00, // l ctrl XXX
    0x04, // a
    0x16, // s
    0x07, // d
    0x09, // f
    0x0a, // g
    0x0b, // h
    0x0d, // j
    0x0e, // k
    0x0f, // l
    0x33, // ;
    0x34, // '
    0x00, // 'key grave'?
    0x00, // l shift XXX
    0x31, // '\'
    0x1d, // z
    0x1b, // x
    0x06, // c
    0x19, // v
    0x05, // b
    0x11, // n
    0x10, // m
    0x36, // ,
    0x37, // .
    0x38, // '/'
    0x00, // r shift XXX
    0x00, // 'key kpasterisk'?
    0x00, // l alt XXX
    0x2c, // spacebar
    0x39, // caps lock
    0x3a, // f1
    0x3b, // f2
    0x3c, // f3
    0x3d, // f4
    0x3e, // f5
    0x3f, // f6
    0x40, // f7
    0x41, // f8
    0x42, // f9
    0x43, // f10
};


int is_down(__u8 *kc_val, __u8 key_code)
{
    int i;
    for(i=2; kc_val[i]!=key_code && i<6; i++);
    return (i<6);
}


int main(int argc, char **argv)
{
    int pos, i, tmp;
    struct input_event ev;
    __u8 kc_val[8];
    __u8 key_code;

    if(argc < 3) return 1;

    int ifd = open(argv[1], O_RDONLY);
    int ofd = open(argv[2], O_WRONLY);
    if(ifd < 0 || ofd < 0) return 1;


    memset(kc_val, 0, 8);
    while(read(ifd, &ev, sizeof(ev)) == sizeof(ev)){
        printf("input event -- type: %u, code: %u, value: %d\n", ev.type, ev.code, ev.value);
        if(ev.type == EV_KEY && ev.code < 246){
            if(ev.value == 1){ // a press-down
                if(ev.code == KEY_LEFTCTRL){
                    kc_val[0] |= 1 << 4;
                }else if(ev.code == KEY_LEFTSHIFT){
                    kc_val[0] |= 1 << 5;
                }else if(ev.code == KEY_LEFTALT){
                    kc_val[0] |= 1 << 6;
                }else if(ev.code == KEY_LEFTMETA){
                    kc_val[0] |= 1 << 7;
                }else if(ev.code == KEY_RIGHTCTRL){
                    kc_val[0] |= 1;
                }else if(ev.code == KEY_RIGHTSHIFT){
                    kc_val[0] |= 1 << 1;
                }else if(ev.code == KEY_RIGHTALT){
                    kc_val[0] |= 1 << 2;
                }else if(ev.code == KEY_RIGHTMETA){
                    kc_val[0] |= 1 << 3;
                }else{
                    /* other keys, check the array */
                    key_code = key_map[ev.code];
                    if(!is_down(kc_val, key_code)){
                        for(pos = 2; kc_val[pos] && pos < 6; pos++);
                        if(pos < 6) kc_val[pos] = key_code;
                    }
                }
                if((tmp = write(ofd, kc_val, 8)) != 8){
                    fprintf(stderr, "write error, len=%d, abort.\n", tmp);
                    break;
                }
            }else if(ev.value == 0){ // a release
                if(ev.code == KEY_LEFTCTRL){
                    kc_val[0] &= ~((__u8)(1<<4));
                }else if(ev.code == KEY_LEFTSHIFT){
                    kc_val[0] &= ~((__u8)(1<<5));
                }else if(ev.code == KEY_LEFTALT){
                    kc_val[0] &= ~((__u8)(1<<6));
                }else if(ev.code == KEY_LEFTMETA){
                    kc_val[0] &= ~((__u8)(1<<7));
                }else if(ev.code == KEY_RIGHTCTRL){
                    kc_val[0] &= ~((__u8)1);
                }else if(ev.code == KEY_RIGHTSHIFT){
                    kc_val[0] &= ~((__u8)(1<<1));
                }else if(ev.code == KEY_RIGHTALT){
                    kc_val[0] &= ~((__u8)(1<<2));
                }else if(ev.code == KEY_RIGHTMETA){
                    kc_val[0] &= ~((__u8)(1<<3));
                }else{
                    /* other keys, check the array */
                    key_code = key_map[ev.code];
                    if(is_down(kc_val, key_code)){
                        for(pos = 2; kc_val[pos] != key_code && pos < 6; pos++);
                        if(pos < 6){
                            for(i = pos; i < 5; i++)
                                kc_val[i] = kc_val[i+1];
                            kc_val[i] = 0;
                        }
                    }
                }
                if((tmp = write(ofd, kc_val, 8)) != 8){
                    fprintf(stderr, "write error, len=%d, abort.\n", tmp);
                    break;
                }
            }else continue;
        }
    }

    return 1;
}
