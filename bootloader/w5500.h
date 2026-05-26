#ifndef W5500_H
#define W5500_H
#include <stdint.h>

/* SPI MMIO base. Layout (word offsets):
 *   +0x00 DATA  (W8/R8) — write byte to start transfer, read byte to get RX.
 *   +0x04 CTRL  bit0=busy(RO), bit1=CS_N, bit2=soft_reset(W1), bit3=RST_N(W5500),
 *                bits[15:8]=clkdiv. */
#define SPI_BASE    0x10000060u
#define SPI_DATA    (*(volatile uint32_t *)(SPI_BASE + 0x00))
#define SPI_CTRL    (*(volatile uint32_t *)(SPI_BASE + 0x04))

/* W5500 control bits in CTRL register. */
#define CTRL_BUSY      0x01u
#define CTRL_CS_DEASS  0x02u    /* set CS_N=1 (idle) */
#define CTRL_SOFT_RST  0x04u    /* W1: reset SPI core */
#define CTRL_W5500_RUN 0x08u    /* set RST_N=1 (W5500 out of reset) */

/* W5500 register blocks (BSB bits[7:3] of control byte). */
#define W5500_BSB_COMMON   0x00
#define W5500_BSB_SOCK0_R  0x01
#define W5500_BSB_SOCK0_TX 0x02
#define W5500_BSB_SOCK0_RX 0x03

/* W5500 Common Registers (block=0x00). */
#define W5500_MR        0x0000   /* mode */
#define W5500_GAR       0x0001   /* gateway IP (4B) */
#define W5500_SUBR      0x0005   /* subnet mask (4B) */
#define W5500_SHAR      0x0009   /* source MAC (6B) */
#define W5500_SIPR      0x000F   /* source IP (4B) */
#define W5500_VERSIONR  0x0039   /* should read 0x04 */

/* Socket Register block offsets (block=0x01,5,9,... for sockets 0-7). */
#define W5500_Sn_MR     0x0000
#define W5500_Sn_CR     0x0001
#define W5500_Sn_IR     0x0002
#define W5500_Sn_SR     0x0003
#define W5500_Sn_PORT   0x0004
#define W5500_Sn_DIPR   0x000C
#define W5500_Sn_DPORT  0x0010

/* W5500 socket commands (Sn_CR). */
#define W5500_CR_OPEN     0x01
#define W5500_CR_LISTEN   0x02
#define W5500_CR_CONNECT  0x04
#define W5500_CR_DISCON   0x08
#define W5500_CR_CLOSE    0x10
#define W5500_CR_SEND     0x20
#define W5500_CR_RECV     0x40

/* Sn_MR mode. */
#define W5500_MR_TCP      0x01

/* Sn_SR status. */
#define W5500_SR_CLOSED       0x00
#define W5500_SR_INIT         0x13
#define W5500_SR_LISTEN       0x14
#define W5500_SR_ESTABLISHED  0x17
#define W5500_SR_CLOSE_WAIT   0x1C
#define W5500_SR_SYNSENT      0x15
#define W5500_SR_SYNRECV      0x16
#define W5500_SR_FIN_WAIT     0x18
#define W5500_SR_CLOSING      0x1A
#define W5500_SR_TIME_WAIT    0x1B
#define W5500_SR_LAST_ACK     0x1D

/* Socket 0 register block selectors. */
#define W5500_S0_REG   0x01
#define W5500_S0_TX    0x02
#define W5500_S0_RX    0x03

/* --- low-level driver --- */
void     w5500_reset(void);              /* power-cycle reset (>1ms) */
uint8_t  w5500_read_reg(uint16_t addr, uint8_t bsb);
void     w5500_write_reg(uint16_t addr, uint8_t bsb, uint8_t value);
void     w5500_read_buf(uint16_t addr, uint8_t bsb, uint8_t *buf, uint16_t len);
void     w5500_write_buf(uint16_t addr, uint8_t bsb, const uint8_t *buf, uint16_t len);
uint16_t w5500_read16(uint16_t addr, uint8_t bsb);
void     w5500_write16(uint16_t addr, uint8_t bsb, uint16_t value);
uint8_t  w5500_version(void);            /* returns VERSIONR — should be 0x04 */

/* --- network config --- */
void     w5500_set_mac(const uint8_t mac[6]);
void     w5500_set_ip(const uint8_t ip[4]);
void     w5500_set_gateway(const uint8_t gw[4]);
void     w5500_set_subnet(const uint8_t sub[4]);

/* --- socket 0 TCP --- */
int      w5500_tcp_open(uint16_t local_port);            /* 0 = OK, -1 = fail */
int      w5500_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port);
int      w5500_tcp_send(const uint8_t *data, uint16_t len);
int      w5500_tcp_recv(uint8_t *buf, uint16_t max);     /* returns bytes received */
void     w5500_tcp_close(void);
uint8_t  w5500_tcp_status(void);                         /* returns Sn_SR */

#endif
