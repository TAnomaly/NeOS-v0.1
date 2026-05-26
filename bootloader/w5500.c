#include "w5500.h"

extern void puts_both(const char *s);

static void spi_wait(void) {
    while (SPI_CTRL & CTRL_BUSY) { }
}

static uint8_t spi_xfer(uint8_t b) {
    SPI_DATA = b;
    spi_wait();
    return (uint8_t)(SPI_DATA & 0xFF);
}

static void spi_cs_lo(void) {
    SPI_CTRL = CTRL_W5500_RUN;     /* CS_N=0, RST_N=1 */
}

static void spi_cs_hi(void) {
    SPI_CTRL = CTRL_W5500_RUN | CTRL_CS_DEASS;   /* CS_N=1, RST_N=1 */
}

void w5500_reset(void) {
    /* CS_N=1, RST_N=0 (W5500 in reset). */
    SPI_CTRL = CTRL_CS_DEASS;
    for (volatile uint32_t i = 0; i < 50000; i++) { }
    /* RST_N=1 (running), CS_N=1. */
    SPI_CTRL = CTRL_CS_DEASS | CTRL_W5500_RUN;
    /* Settle for PLL lock. */
    for (volatile uint32_t i = 0; i < 500000; i++) { }
}

/* Frame format: addr_hi, addr_lo, control(BSB<<3 | RW<<2 | OM), data... */
uint8_t w5500_read_reg(uint16_t addr, uint8_t bsb) {
    uint8_t ctl = (uint8_t)((bsb << 3) & 0xF8);   /* read=0, OM=0 */
    spi_cs_lo();
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)(addr & 0xFF));
    spi_xfer(ctl);
    uint8_t v = spi_xfer(0x00);
    spi_cs_hi();
    return v;
}

void w5500_write_reg(uint16_t addr, uint8_t bsb, uint8_t value) {
    uint8_t ctl = (uint8_t)(((bsb << 3) & 0xF8) | 0x04);   /* write=1 */
    spi_cs_lo();
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)(addr & 0xFF));
    spi_xfer(ctl);
    spi_xfer(value);
    spi_cs_hi();
}

void w5500_read_buf(uint16_t addr, uint8_t bsb, uint8_t *buf, uint16_t len) {
    uint8_t ctl = (uint8_t)((bsb << 3) & 0xF8);   /* read=0, OM=0 (variable) */
    spi_cs_lo();
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)(addr & 0xFF));
    spi_xfer(ctl);
    for (uint16_t i = 0; i < len; i++) buf[i] = spi_xfer(0x00);
    spi_cs_hi();
}

void w5500_write_buf(uint16_t addr, uint8_t bsb, const uint8_t *buf, uint16_t len) {
    uint8_t ctl = (uint8_t)(((bsb << 3) & 0xF8) | 0x04);   /* write=1 */
    spi_cs_lo();
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)(addr & 0xFF));
    spi_xfer(ctl);
    for (uint16_t i = 0; i < len; i++) spi_xfer(buf[i]);
    spi_cs_hi();
}

uint16_t w5500_read16(uint16_t addr, uint8_t bsb) {
    uint8_t b[2];
    w5500_read_buf(addr, bsb, b, 2);
    return ((uint16_t)b[0] << 8) | b[1];
}

void w5500_write16(uint16_t addr, uint8_t bsb, uint16_t value) {
    uint8_t b[2] = { (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    w5500_write_buf(addr, bsb, b, 2);
}

uint8_t w5500_version(void) {
    return w5500_read_reg(W5500_VERSIONR, W5500_BSB_COMMON);
}

/* --- network config --- */
void w5500_set_mac(const uint8_t mac[6]) {
    w5500_write_buf(W5500_SHAR, W5500_BSB_COMMON, mac, 6);
}
void w5500_set_ip(const uint8_t ip[4]) {
    w5500_write_buf(W5500_SIPR, W5500_BSB_COMMON, ip, 4);
}
void w5500_set_gateway(const uint8_t gw[4]) {
    w5500_write_buf(W5500_GAR, W5500_BSB_COMMON, gw, 4);
}
void w5500_set_subnet(const uint8_t sub[4]) {
    w5500_write_buf(W5500_SUBR, W5500_BSB_COMMON, sub, 4);
}

/* --- socket 0 TCP --- */

/* Socket 0 register addresses (block-relative). */
#define Sn_MR        0x0000
#define Sn_CR        0x0001
#define Sn_IR        0x0002
#define Sn_SR        0x0003
#define Sn_PORT      0x0004
#define Sn_DIPR      0x000C
#define Sn_DPORT     0x0010
#define Sn_TX_FSR    0x0020   /* TX free size (2B) */
#define Sn_TX_RD     0x0022
#define Sn_TX_WR     0x0024
#define Sn_RX_RSR    0x0026   /* RX received size (2B) */
#define Sn_RX_RD     0x0028
#define Sn_RX_WR     0x002A

static void s0_cmd(uint8_t cmd) {
    w5500_write_reg(Sn_CR, W5500_S0_REG, cmd);
    /* Wait for command to clear (W5500 zeroes Sn_CR when accepted). */
    while (w5500_read_reg(Sn_CR, W5500_S0_REG) != 0) { }
}

uint8_t w5500_tcp_status(void) {
    return w5500_read_reg(Sn_SR, W5500_S0_REG);
}

int w5500_tcp_open(uint16_t local_port) {
    /* Close any existing socket. */
    s0_cmd(W5500_CR_CLOSE);
    /* Set mode = TCP. */
    w5500_write_reg(Sn_MR, W5500_S0_REG, W5500_MR_TCP);
    /* Set local source port. */
    w5500_write16(Sn_PORT, W5500_S0_REG, local_port);
    /* Open. */
    s0_cmd(W5500_CR_OPEN);
    /* Verify SOCK_INIT. */
    if (w5500_tcp_status() != W5500_SR_INIT) return -1;
    return 0;
}

int w5500_tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port) {
    w5500_write_buf(Sn_DIPR, W5500_S0_REG, dst_ip, 4);
    w5500_write16(Sn_DPORT, W5500_S0_REG, dst_port);
    s0_cmd(W5500_CR_CONNECT);
    /* Wait for ESTABLISHED or CLOSED (failure). Up to ~3s. */
    for (uint32_t t = 0; t < 3000000; t++) {
        uint8_t s = w5500_tcp_status();
        if (s == W5500_SR_ESTABLISHED) return 0;
        if (s == W5500_SR_CLOSED)      return -1;
        for (volatile int d = 0; d < 100; d++) { }
    }
    return -1;
}

int w5500_tcp_send(const uint8_t *data, uint16_t len) {
    /* Wait until TX FIFO has room. */
    uint16_t free;
    while ((free = w5500_read16(Sn_TX_FSR, W5500_S0_REG)) < len) {
        uint8_t s = w5500_tcp_status();
        if (s != W5500_SR_ESTABLISHED && s != W5500_SR_CLOSE_WAIT) return -1;
    }
    uint16_t wr_ptr = w5500_read16(Sn_TX_WR, W5500_S0_REG);
    /* Write into TX buffer at addr=wr_ptr (block = S0_TX). */
    w5500_write_buf(wr_ptr, W5500_S0_TX, data, len);
    /* Advance write pointer. */
    w5500_write16(Sn_TX_WR, W5500_S0_REG, (uint16_t)(wr_ptr + len));
    /* SEND command. */
    s0_cmd(W5500_CR_SEND);
    return (int)len;
}

int w5500_tcp_recv(uint8_t *buf, uint16_t max) {
    uint16_t rsr = w5500_read16(Sn_RX_RSR, W5500_S0_REG);
    if (rsr == 0) return 0;
    uint16_t n = (rsr > max) ? max : rsr;
    uint16_t rd_ptr = w5500_read16(Sn_RX_RD, W5500_S0_REG);
    w5500_read_buf(rd_ptr, W5500_S0_RX, buf, n);
    w5500_write16(Sn_RX_RD, W5500_S0_REG, (uint16_t)(rd_ptr + n));
    s0_cmd(W5500_CR_RECV);
    return (int)n;
}

void w5500_tcp_close(void) {
    s0_cmd(W5500_CR_CLOSE);
}
