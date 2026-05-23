# NeOS — Sıfırdan Bir Mini İşletim Sistemi Yolculuğu

Bu belge, **27 dolarlık bir Tang Nano 9K FPGA kartı** üzerinde sıfırdan
inşa ettiğimiz **NeOS** adındaki minik işletim sisteminin nasıl
ortaya çıktığını adım adım anlatır. Her bölüm bağımsız olarak
okunabilir; her şey, hiçbir önceki bilgi varsayılmadan açıklanmıştır.

---

## İçindekiler

1. [Donanım — Tang Nano 9K nedir?](#1-donanım---tang-nano-9k-nedir)
2. [Açık kaynak araç zinciri](#2-açık-kaynak-araç-zinciri)
3. [picorv32 — soft RISC-V CPU](#3-picorv32--soft-risc-v-cpu)
4. [İlk SoC: CPU + RAM + UART + LED](#4-i̇lk-soc-cpu--ram--uart--led)
5. [İlk C programı: derleyip flash etmek](#5-i̇lk-c-programı-derleyip-flash-etmek)
6. [UART çift yönlü hale getirme](#6-uart-çift-yönlü-hale-getirme)
7. [UART bootloader: hızlı geliştirme döngüsü](#7-uart-bootloader-hızlı-geliştirme-döngüsü)
8. [HDMI text terminal (SVO)](#8-hdmi-text-terminal-svo)
9. [NeOS — REPL shell](#9-neos--repl-shell)
10. [Mini-C yorumlayıcı](#10-mini-c-yorumlayıcı)
11. [Mini-C derleyici (compile-and-run)](#11-mini-c-derleyici-compile-and-run)
12. [Eğlenceli uygulamalar](#12-eğlenceli-uygulamalar)
13. [Denemeler ve duvarlar (PSRAM, HDMI audio)](#13-denemeler-ve-duvarlar-psram-hdmi-audio)
14. [Bundan sonra ne olacak?](#14-bundan-sonra-ne-olacak)

---

## 1. Donanım — Tang Nano 9K nedir?

**Tang Nano 9K**, Çinli üretici Sipeed'in yaptığı yaklaşık 27 dolarlık
küçük bir FPGA geliştirme kartıdır. Üzerinde:

- **Gowin GW1NR-LV9 QN88** FPGA çipi (yaklaşık 8.800 LUT, 468 Kbit
  yerleşik BRAM, dahili paket-içi 8 MB HyperRAM)
- **HDMI konektörü** — TMDS pinleri doğrudan FPGA'ya bağlı
- **USB-C** — BL616 köprü çipi üzerinden hem JTAG hem UART
- **6 kullanıcı LED'i**, **2 düğme** (S1=reset, S2=user)
- **16 MB SPI Flash** — bitstream burada saklanır, geri kalanı boştur
- 27 MHz sabit osilatör

FPGA "soft logic" = istediğin devreyi LUT'lardan inşa edersin. Biz de
bu LUT'lardan **RISC-V tabanlı bir CPU + UART + LED + HDMI** kuracağız.

---

## 2. Açık kaynak araç zinciri

Çoğu FPGA satıcısı kapalı, ücretli IDE'lerle gelir. Tang Nano 9K için
**tamamen açık kaynak** bir akış mevcut:

| Araç | İş |
|------|----|
| **Yosys** | Verilog'u netliste sentezler |
| **nextpnr-himbaechel** | Yerleştirme + yönlendirme (place & route) |
| **gowin_pack** (Apicula) | Yönlendirilmiş netlisti bitstream'e paketler |
| **openFPGALoader** | Bitstream'i kartın flash'ına yazar |
| **riscv64-unknown-elf-gcc** | C kodunu RV32 makine koduna derler |

Hepsini **oss-cad-suite** paketinden alabilirsin. Hiçbir vendor IDE'sine
bağımlılık yok. Projeyi kurmak için tek tek konfigürasyon yapmak yerine
şu basit `Makefile.oss` yetiyor:

```makefile
SOURCES := src/top.v src/soc.v src/picorv32.v src/ram.v \
           src/uart_tx.v src/uart_rx.v ...

build_oss/picorv32_soc.fs: $(SOURCES)
    yosys -p "read_verilog $(SOURCES); synth_gowin -top top -json $@.json"
    nextpnr-himbaechel --json $@.json --write $@.pnr.json \
        --device GW1NR-LV9QN88PC6/I5 --vopt family=GW1N-9C --vopt cst=tangnano9k.cst
    gowin_pack -d GW1N-9C -o $@ $@.pnr.json

flash:
    openFPGALoader -b tangnano9k -f build_oss/picorv32_soc.fs
```

---

## 3. picorv32 — soft RISC-V CPU

**picorv32**, Clifford Wolf tarafından yazılan, RV32IMC ISA'yı destekleyen
açık kaynak bir RISC-V çekirdeğidir. Bizim kullandığımız özellikler:

- `ENABLE_MUL=1` — donanım çarpma
- `ENABLE_DIV=1` — donanım bölme (illegal instruction'dan kaçınmak için
  şart, gcc otomatik `divu` üretiyordu)
- `COMPRESSED_ISA=1` — 16-bit sıkıştırılmış komutlar, kod boyutunu küçültür
- `STACKADDR=0x8000` — yığın 32 KB BRAM'in tepesinden başlar
- `PROGADDR_RESET=0x0000` — reset vector'ü 0'da başlar

CPU yaklaşık 2.000 LUT kullanır, geri kalan ~6.800 LUT diğer çevre
birimleri için kalır.

---

## 4. İlk SoC: CPU + RAM + UART + LED

Tek bir top-level Verilog modülü etrafında parçaları birleştirdik:

```
       ┌──────────────────────────────────┐
       │   top.v                          │
       │                                  │
       │  ┌─────────┐  ┌──────────────┐  │
       │  │picorv32 │──│  soc.v       │  │
       │  │ (CPU)   │  │  - addr dec  │  │
       │  └─────────┘  │  - mem ready │  │
       │               └────┬─────────┘  │
       │                    ▼            │
       │      ┌──────┐  ┌──────┐  ┌────┐│
       │      │ ram  │  │ uart │  │led ││
       │      │ 32K  │  │ tx/rx│  │6bit││
       │      └──────┘  └──────┘  └────┘│
       └──────────────────────────────────┘
```

**Memory map:**

| Adres | Çevre birimi |
|-------|--------------|
| `0x00000000-0x00007FFF` | RAM 32 KB |
| `0x10000000` | UART_TX_DATA (write) |
| `0x10000004` | UART_STATUS (read: tx_busy + rx_valid) |
| `0x10000008` | UART_RX_DATA (read, clear-on-read) |
| `0x10000010` | LED (write, 6-bit) |

picorv32'nin memory interface'i basit: `mem_valid` aktif olunca adres
çözülür, `mem_ready` ile transfer tamamlanır. Tek cycle ready için:

```verilog
always @(posedge clk) begin
    if (reset)             mem_ready_r <= 0;
    else if (mem_ready_r)  mem_ready_r <= 0;
    else if (mem_valid)    mem_ready_r <= 1;
end
```

---

## 5. İlk C programı: derleyip flash etmek

C kodu yazıyoruz, gcc ile RV32IM'e derliyoruz, ham binary'i hex'e
çeviriyoruz, BRAM'in `$readmemh` ile başlangıç içeriği olarak yüklüyoruz.

```c
#include <stdint.h>
#define LED_REG (*(volatile uint32_t *)0x10000010)
int main(void) {
    for (uint32_t i = 0;; i++) {
        LED_REG = i & 0x3F;
        for (volatile uint32_t d = 0; d < 200000; d++);
    }
}
```

Build döngüsü ilk sürümde:

```bash
cd firmware && make           # gcc + objcopy + bin2hex → firmware.hex
make -f Makefile.oss          # yosys/nextpnr/gowin_pack (firmware BRAM'e gömülü)
make -f Makefile.oss flash    # bitstream'i flash'a yaz
```

LED'ler artıyor. İlk yaşam belirtisi.

---

## 6. UART çift yönlü hale getirme

Başlangıçtaki `soc.v` `uart_tx` modülünü hardcoded "A" karakteri ile bir
debug sayacından besliyordu — CPU'nun yazdığı baytlar pin'e ulaşmıyordu.
Tek tıklamada düzelttik:

```verilog
wire uart_tx_we = mem_valid && sel_uart && (mem_addr[3:2]==00)
                  && (mem_wstrb != 0) && !mem_ready_r;
uart_tx u_tx (.clk(clk), .reset(reset), .we(uart_tx_we),
              .data(mem_wdata[7:0]), .tx(uart_tx), .busy(uart_tx_busy));
```

Sonra `uart_rx.v` modülünü yazdık: 2-FF synchronizer + start-bit
algılama + 16x oversample + sticky valid bayrağı + read-ack ile temizleme.

C tarafında `getchar_nb()` ile non-blocking okuma mümkün.

---

## 7. UART bootloader: hızlı geliştirme döngüsü

İlk başta her C değişikliği için **tüm bitstream yeniden sentezlenmek
zorundaydı** (~30 saniye). Çözüm: minik bir **UART bootloader** yazdık.

Akış:

1. Bootloader BRAM'in başında durur (`0x0000`).
2. Açılışta `BOOT>` prompt verir.
3. Host script (`tools/upload.py`) UART üzerinden:
   - `u` gönderir
   - 4 byte size + N byte payload + 4 byte checksum
4. Bootloader payload'u `0x6000`'e yazar, checksum doğrularsa `0x6000`'e
   jump eder.

Sonuç: **C değişikliği → 3 saniyede uygulama çalışıyor**. Bitstream
sadece bootloader değişince yeniden sentez gerek.

```bash
$ ./upload_app.sh
[*] firmware.bin: 332 bytes, sum=0x00005f36
[*] sending 'u' / sz? / payload / checksum
[+] bootloader: OK run
Hello, world!
```

---

## 8. HDMI text terminal (SVO)

Tang Nano 9K'nin HDMI konektörü doğrudan FPGA'ya bağlı. **TMDS sinyalini
FPGA içinde üretiriz**:

- PLL ile 27 MHz → 126 MHz, sonra `/5` ile 25.2 MHz piksel saati
  (640×480 @ 60 Hz timing)
- TMDS encoder × 3 kanal (RGB)
- `OSER10` (10:1 serializer) ile 252 Mbps her kanal
- `ELVDS_OBUF` ile differential output

Clifford Wolf'un **SVO (Simple Video Out)** çatısı içerisinde:
- `svo_term` — UART benzeri byte stream'ini 80×30 text buffer'a yazar,
  font ROM'la her piksel için fore/back/no-char belirler
- `svo_overlay` — text'i video stream'in üstüne karıştırır
- `svo_enc` + `svo_tmds` — video timing + TMDS encoding

CPU sadece `term_in_tvalid + term_in_tdata` ile byte yazar:

```c
#define TERM_DATA   (*(volatile uint32_t *)0x10000020)
#define TERM_STATUS (*(volatile uint32_t *)0x10000024)
static void term_putc(char c) {
    while (TERM_STATUS & 1);
    TERM_DATA = c;
}
```

Sonuç: siyah arka plan üzerine parlak yeşil text monitörde görünür.

---

## 9. NeOS — REPL shell

Bootloader'ı genişletip **NeOS** adını verdik. Açılışta ASCII art splash
gösterir, sonra `NeOS> ` prompt'u. Eski "upload-only" akış üzerine
gerçek bir komut sistemi ekledik.

Klasik komutlar:

| Komut | İş |
|-------|-----|
| `help` | komut listesi |
| `clear` | ekranı temizle |
| `led <hex>` | LED bit pattern yaz |
| `peek <addr>` | 32-bit word oku |
| `poke <addr> <val>` | 32-bit word yaz |
| `dump <addr> <len>` | hex dump |
| `mul <a> <b>` | decimal çarp |
| `hex <dec>` | dec → hex |
| `u` | UART'tan binary yükle |
| `g` veya `run` | yüklü binary'yi çalıştır |
| `info` | sistem bilgisi |
| `ascii` | ASCII tablo |

Her komut hem UART'a hem HDMI'ye paralel yazılır (`puts_both`).

---

## 10. Mini-C yorumlayıcı

Bir adım daha: NeOS prompt'unda **kullanıcı C ifadesi yazsın, anında
yorumlanıp sonuç dönsün** istedik.

`bootloader/interp.c` — yaklaşık 600 satırlık recursive descent parser:

- Operatörler: `+ - * / % & | ^ << >> < <= > >= == != && || ! ~`
- Değişkenler: `a-z, A-Z` (52 int slot)
- `let x = EXPR` / `x = EXPR`
- `print EXPR` / `print "string"` / `printh EXPR`
- Bare expression: `5+7*2` → `19`
- Built-in fonksiyonlar: `led(v)`, `peek(a)`, `poke(a,v)`,
  `delay(ms)`, `read()`, `write(b)`, `tone(hz)`

Örnek:

```
NeOS> let x = 7*13
x = 91
NeOS> print x*x
8281
NeOS> printh x
0x0000005B
NeOS> led(0x2A)
led=2a
NeOS> print "Wake up Neo The Matrix Has You"
Wake up Neo The Matrix Has You
```

Tüm bunlar **yorumlanır**, derlenmez. Bir sonraki seviye için derleyici
yazdık.

---

## 11. Mini-C derleyici (compile-and-run)

`bootloader/cc.c` — yaklaşık 800 satırlık **gerçek bir mini-C
derleyicisi**. Kullanıcı yazdığı C kodunu **anında RV32IM makine
koduna derleyip RAM'e yerleştirir, jump eder, çalıştırır**.

**Mimari:**

1. **Tokenizer** — anahtar kelimeler (`int if else while`),
   tanımlayıcılar, sayılar, string literal, operatörler.
2. **Parser** — recursive descent, precedence climbing.
3. **Code generator** — tek geçişte, her statement için doğrudan
   RV32IM komutu emit eder.
4. **Syscall table** — `SYSCALL_BASE = 0x6080`. Derlenmiş kod
   `led(v)`, `print(n)`, `puts("...")`, `delay(ms)` gibi
   builtin'leri buradan çağırır.

**Memory map (compile modu):**

```
0x0000-0x5FFF  Bootloader (24KB) — NeOS + interpreter + compiler
0x6000-0x607F  Compiled-code int variable storage (32 slot)
0x6080-0x60FF  Syscall function pointer table
0x6100-0x6FFF  Compiled code buffer (~960 instructions)
0x7000-0x7FFF  Geleneksel app slot
```

**Codegen stratejisi:** stack-machine. Result her zaman `a0`'da.
Binary operation için: sol operandı hesapla → push a0 → sağ operandı
hesapla → pop a1 → işlem yap.

**Desteklenen alt küme:**
- `int` değişkenler (tek harfli isim)
- `if/else/while` kontrol akışı
- Aritmetik + karşılaştırma + bitwise + kaydırma
- Builtin çağrıları (syscall table üzerinden)
- String literal (sadece `puts` içinde)

**Kullanım:**

```
NeOS> cc print(7*13);
[cc] 14 instr
91
[cc] done

NeOS> cc int x=0; while (x<6) { led(1<<x); delay(150); x=x+1; }
[cc] 38 instr
(LED bar 6 adımda kayar)
[cc] done
```

Bu **literally NeOS içinde derleyici** — kullanıcı C yazar, NeOS o C'yi
makine koduna çevirir, jump eder, donanım üzerinde çalışır. Mecrisp
Forth'un C syntax'lı eşdeğeri.

---

## 12. Eğlenceli uygulamalar

`bootloader/main.c` içinde NeOS komutları olarak:

| Komut | İş |
|-------|-----|
| `mandel` | 8.8 fixed-point ASCII Mandelbrot |
| `matrix` | Matrix-rain animasyonu (tuş ile dur) |
| `pong` | AI-vs-AI text Pong demosu |
| `guess` | 1-100 sayı tahmin oyunu |
| `hangman` | Kelime tahmin oyunu |
| `ascii` | ASCII tablo |
| `info` | Sistem bilgisi |

Hepsi hem UART'ta hem HDMI'de görünür. Hepsi C'de bootloader'a gömülü
(külte ek RAM'e veya derlemeye gerek yok).

---

## 13. Denemeler ve duvarlar (PSRAM, HDMI audio)

Her şey tatlı değildi. Birkaç ciddi denemede donanım duvarlarına
çarptık:

### HDMI audio (HDMI 1.4 data island packets)

500+ satırlık TMDS + TERC4 + BCH ECC + paket scheduler yazdık.
İlk denemede video sinyali bozdu, monitör "no signal" oldu. Pipeline
alignment + ctrl bit kanal hataları tespit edildi, kısmen düzeltildi
ama compile-able + functional video yine yokoldu. Sebep: data island
periyodları HDMI sink chip'inin çok kesin timing beklentisiyle uyuşmadı,
HDMI analyzer olmadan debug imkansız. **Bırakıldı.**

### Tang Nano 9K on-die HyperRAM (PSRAM) + HDMI

Gowin IDE proprietary IP'siyle denedik. Buldukları:

1. **Yosys/nextpnr-himbaechel** Tang Nano 9K'nin on-die HyperRAM
   pinlerini desteklemiyor (IOLOGIC packing assertion failure).
2. **Gowin IDE flow** ise build edebiliyor ama HyperRAM Bank 1'i
   1.8V'a kilitliyor, HDMI ise pin 69-75'te 3.3V LVCMOS33D
   gerektiriyor. **Donanımsal çakışma — fiziksel imkansız.**

Çözümler:
- HDMI feda et → PSRAM al (HDMI ile uyumlu değil)
- External SPI PSRAM modülü (ek donanım)
- Tang Nano 20K board (HDMI + PSRAM ayrı banklarda)

Şimdilik: HDMI tercih edildi, 32 KB BRAM ile devam.

### gcc/TinyCC port

TinyCC ~10 MB RAM ister. 32 KB BRAM'a sığmaz. Yerine **kendi mini-C
derleyicimizi yazdık** (`cc.c`) — küçük ama gerçek bir derleyici,
NeOS içinde çalışır.

---

## 14. Bundan sonra ne olacak?

Tang Nano 9K + HDMI içinde yapılabilecekler:

- **`printf` formatlı çıktı** (`%d %s %x`) — quality of life
- **SPI Flash dosya sistemi** — 16 MB on-board flash'ın boş kısmına
  yazılabilir/okunabilir mini FS
- **Multi-line editor `ed`** — NeOS içinde script yaz, FS'ye kaydet
- **Komut geçmişi (↑↓)** — ANSI escape parse
- **Mini-C compiler v0.2** — user fonksiyonları, daha iyi error
- **Snake, Tetris, 2048** — gerçek oyunlar
- **Cooperative scheduler** — birden fazla task
- **Crash handler** — illegal instruction trap → register dump
- **`bench`** — CPU benchmark
- **`maze` + DFS solver** — random labirent

Tang Nano 20K'ya geçilirse:
- HDMI + 8 MB HyperRAM aynı anda
- TinyCC port mümkün
- Gerçek gcc-like derleyici, dosya tabanlı çalışma

---

## Memory haritası (özet)

```
0x00000000-0x00005FFF  Bootloader/NeOS (24 KB) — kod + bss + stack
0x00006000-0x0000607F  Mini-C variable storage (128 byte)
0x00006080-0x000060FF  Syscall pointer table (128 byte)
0x00006100-0x00006FFF  Compiled-code buffer (3.75 KB / 960 instr)
0x00007000-0x00007FFF  App upload slot (eski 'u' komutu için)
0x10000000             UART_TX_DATA
0x10000004             UART_STATUS
0x10000008             UART_RX_DATA
0x10000010             LED
0x10000020             TERM_DATA (HDMI text)
0x10000024             TERM_STATUS
0x10000030             AUDIO_FREQ (placeholder, donanım yok)
```

---

## Kapanış

Tek satırlık `let x = 7*13` ifadesi NeOS'ta yazıldığında, arkada şunlar
oluyor:

1. UART üzerinden BL616 USB köprüsünden FPGA'ya byte byte gelir
2. `uart_rx.v` 2-FF sync + start bit + 8 data + stop'u decode eder
3. picorv32 mem peripheral'dan okur, `read_line` buffer'a yazar
4. NeOS dispatch'i interpreter'a yönlendirir
5. Recursive descent parser ifadeyi parse eder, mul instruction
   üretip `a0`'a 91 koyar
6. `print_dec_signed` → put_dec → UART_TX_DATA + TERM_DATA byte byte
7. UART tarafı host picocom'da, HDMI tarafı svo_term text buffer'a
   yazar, font ROM ile piksel olarak render edilir, TMDS ile
   monitöre gider

**Hepsi tek FPGA'da, tek osilatörle, ~27 dolarlık board üzerinde,
tamamen açık kaynak.**

Modern bir Raspberry Pi alıp Linux'ta `python3 -c "print(7*13)"`
yazmak yerine **her katmanı kendin inşa edip bunu çalıştırmak** —
işte bunun değeri bilgisayarın *gerçekten* nasıl çalıştığını
hissetmektir.
