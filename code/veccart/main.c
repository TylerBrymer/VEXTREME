/*
 *  Copyright (C) 2015 Jeroen Domburg <jeroen at spritesmods.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/syscfg.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/pwr.h>
#include <libopencm3/stm32/flash.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"
#include "led.h"
#include "delay.h"
#include "main.h"
#include "menu.h"
#include "msc.h"

//#include "rom.h"
#include "xprintf.h"
#include "fatfs/ff.h"


//Memory for the menu ROM and the running cartridge.
//We keep both in memory so we can quickly exchange them when a reset has been detected.
const int menuIndex = 0xfff; // fixed location in multicart.bin
char menuData[8*1024];
char *romData=menuData;
unsigned char parmRam[256];

char menuDir[_MAX_LFN + 1];

union cart_and_listing {
	char cartData[64*1024];
	dir_listing listing;
};

system_options sys_opt;

union cart_and_listing c_and_l;

/*
//Pinning:
A0-A14,SWCTL - PC0-PC15
D0-D7 - PA0-PA7
nWR - PB1
nCART - PB15
led - PB0
USBPWR - PA9
*/

#define SYSCFG_MEMRMP			MMIO32(SYSCFG_BASE + 0x00)


void uart_output_func(unsigned char c){
	uint32_t reg;
	do {
		reg = USART_SR(USART1);
	} while ((reg & USART_SR_TXE) == 0);
	USART_DR(USART1) = (uint16_t) c & 0xff;
}

//Asm function
extern void romemu(void);

//Load a ROM into cartridge memory
void loadRom(char *fn) {
	FIL f;
	FRESULT fr;
	UINT r = 0;
	int n;
	UINT x;
	if (romData == c_and_l.cartData) {
		// loaded cart data
		n = sizeof(c_and_l.cartData);
	} else {
		// loaded menu data
		n = sizeof(menuData);
	}
	fr = f_open(&f, fn, FA_READ);
	if (fr) {
		xprintf("Error opening file: %d\n", fr);
	} else {
		xprintf("Opened file: %s\n", fn);
	}
	f_read(&f, romData, 64*1024, &r);
	xprintf("Read %d bytes of rom data.\n", r);
	// It's a game
	if (n > 32*1024 && r <= 32*1024) {
		// pad with 0x01 for Mine Storm II and Polar Rescue (and any
		// other buggy game that reads outside its program space)
		for (x = r; x < 32*1024; x++) {
			romData[x] = 1;
		}
		xprintf("Padded remaining %d bytes of rom data with 0x01\n", x - r);
		//Duplicate bank to upper bank
		for (n = 0; n < 32*1024; n++) {
			romData[n+32*1024] = romData[n];
		}
	}
	// It's the menu, patch in the HW/SW versions
	else {
		char* ptr1 = strstr(menuData, "11");
		char* ptr2 = strstr(menuData, "22");
		char* ptr3 = strstr(menuData, "33");
		char* ptr4 = strstr(menuData, "44");
		if (ptr1 && ptr2 && ptr3 && ptr4) {
			*ptr1++ = 'V';
			*ptr1   = '0' + (sys_opt.hw_ver >> 8) % 10;
			*ptr2++ = '0' + (sys_opt.hw_ver & 0xFF) / 10;
			*ptr2   = '0' + (sys_opt.hw_ver & 0xFF) % 10;
			*ptr3++ = 'V';
			*ptr3   = '0' + (sys_opt.sw_ver >> 8) % 10;
			*ptr4++ = '0' + (sys_opt.sw_ver & 0xFF) / 10;
			*ptr4   = '0' + (sys_opt.sw_ver & 0xFF) % 10;
		}
	}
	f_close(&f);
}

//Stream data, for the Bad Apple demo
//Name of the file sucks and I'd like to make a rpc function that's a bit
//more universal... you should eg be able to pass through the name of the
//file you'd like to stream and the address and chunk size... but this is
//a start.
FIL streamFile;
int streamLoaded=0;

void loadStreamData(int addr, int len) {
	UINT r=0;
	if (!streamLoaded) {
		f_open(&streamFile, "vec.bin", FA_READ);
		streamLoaded=1;
	}
	f_read(&streamFile, &romData[addr], len, &r);
}

void doUpDir() {
  if (strcmp(menuDir, "/roms") != 0)
    doChangeDir("..");
}

void doChangeDir(char* dirname) {
	xprintf("Found directory: %s\n", dirname);
	if (strcmp(dirname,"..") == 0) {
		char* ptr = strrchr(menuDir,'/');
		if (ptr != NULL) {
			*ptr = '\0';
		}
	} else {
		xsprintf(menuDir, "%s/%s", menuDir, dirname);
	}

	romData=menuData;
	loadListing(menuDir, &c_and_l.listing, menuIndex+1 , menuIndex+1+0x200, romData);
	menuData[menuIndex]=0; //reset selection

	xprintf("Done listing for : %s\n", menuDir);
}

//User has made a selection in the cart menu (chose the i'th item) so now we have to load the cartridge.
void doChangeRom(char* basedir, int i) {
	char buff[300];

	menuData[menuIndex]=i; //save selection so we can go back there after reset
	xprintf("Changing to rom no %d in %s\n", i, basedir);
	sortDirectory(basedir, &c_and_l.listing); // recreate file listing, as loading a cart overwrote the union
	file_entry f = c_and_l.listing.f_entry[i];

	if (f.is_dir) {
		doChangeDir(f.fname);
	} else {													/* It is a file. */
		xprintf("Adding filename [%s] to path\n", f.fname);
		xsprintf(buff, "%s/%s", basedir, f.fname);

		romData=c_and_l.cartData;
		xprintf("Going to read rom image %s\n", buff);
		loadRom(buff);
	}
}

void updateAll() {
	uint16_t i = ledsNumPixels();
	while (i > 0) {                     // color index
		ledsSetPixelColor(--i, colors[(int)parmRam[254]]);
	}
	ledsUpdate();
}

void updateOne() {
	//                led index        , color index
	ledsSetPixelColor((int)parmRam[253], colors[(int)parmRam[254]]);
	ledsUpdate();
}

void updateMulti() {
	// uint16_t i = ledsNumPixels();
	for (uint16_t i = 0; i < ledsNumPixels(); i++) {
		// xprintf("LED%d = %d\n", i, (int)parmRam[0xf0 + i]);
		ledsSetPixelColor(i, colors[(int)parmRam[0xf0 + i]]); // 0xf0 = LED0, 0xf9 = LED9
	}
	// xprintf("\n");
	ledsUpdate();
}

// Load HW/SW versions so that the menu can access and display them
// Warning: This permanently alters the last 4 bytes of 32K ROM game data!
void loadVersions() {
	c_and_l.cartData[0x7ffc] = sys_opt.hw_ver >> 8;
	c_and_l.cartData[0x7ffd] = sys_opt.hw_ver & 0xFF;
	c_and_l.cartData[0x7ffe] = sys_opt.sw_ver >> 8;
	c_and_l.cartData[0x7fff] = sys_opt.sw_ver & 0xFF;
}

// Handle an RPC event
void doHandleEvent(int data) {
	xprintf("Handling Event: %d. arg1: 0x%x... ", data, (int)parmRam[254]);
	switch (data) {
		default:
		case 0: break;
		case 1: doChangeRom(menuDir, (int)parmRam[254]); break;
		case 2: loadStreamData(0x4000, 1024+512); break;
		case 3: doUpDir(); break;
		case 4: updateAll(); break;
		case 5: rainbowStep((int)parmRam[254]); break;
		case 6: updateOne(); break;
		case 7: updateMulti(); break;
		case 8: ledsSetBrightness((int)parmRam[254]); break;
		case 9: loadVersions(); break;
	}
	xprintf("Done\n");
}

void doDbgHook(int adr, int data) {
	xprintf("R %x %x\n", adr, data);
}

static FATFS FatFs;

int main(void) {
	void (*runptr)(void)=romemu;

	const struct rcc_clock_scale hse_8mhz_3v3_120MHz = { /* 120MHz */
		.pllm = 8,
		.plln = 240,
		.pllp = 2,
		.pllq = 5,
		.pllr = 0,
		.pll_source = RCC_CFGR_PLLSRC_HSE_CLK,
		.hpre = RCC_CFGR_HPRE_DIV_NONE,
		.ppre1 = RCC_CFGR_PPRE_DIV_4,
		.ppre2 = RCC_CFGR_PPRE_DIV_2,
		.voltage_scale = PWR_SCALE1,
		.flash_config = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_LATENCY_3WS,
		.ahb_frequency  = 120000000,
		.apb1_frequency = 30000000,
		.apb2_frequency = 60000000,
	};

	rcc_clock_setup_pll(&hse_8mhz_3v3_120MHz);
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_GPIOC);
	rcc_periph_clock_enable(RCC_USART1);
	rcc_periph_clock_enable(RCC_SYSCFG);

	//Addressable LEDs - output
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO13 | GPIO14);
	// gpio_set_output_options(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ,  GPIO13 | GPIO14);
	//LED - output
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO0);
	//USB power - input
	gpio_mode_setup(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN, GPIO9);

	//PB6/PB7: txd/rxd
	gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO6 | GPIO7);
	gpio_set_af(GPIOB, GPIO_AF7, GPIO6 | GPIO7);

	usart_set_baudrate(USART1, 115200);
	usart_set_databits(USART1, 8);
	usart_set_stopbits(USART1, USART_STOPBITS_1);
	usart_set_mode(USART1, USART_MODE_TX);
	usart_set_parity(USART1, USART_PARITY_NONE);
	usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
	/* Finally enable the USART. */
	usart_enable(USART1);
	xdev_out(uart_output_func);

	//Address lines - input (A0 - A14 & PB6)
	gpio_mode_setup(GPIOC, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN,
		GPIO0|GPIO1|GPIO2|GPIO3|GPIO4|GPIO5|GPIO6|GPIO7|GPIO8|GPIO9|GPIO10|GPIO11|GPIO12|GPIO13|GPIO14|GPIO15);
	// gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE,
	// 	GPIO13|GPIO14);
	// IRQ
	gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO9);

	//Data lines - output
	gpio_mode_setup(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
		GPIO0|GPIO1|GPIO2|GPIO3|GPIO4|GPIO5|GPIO6|GPIO7);

	//Control lines - input
	gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE,
		GPIO1|GPIO15);

	// SysTick setup (calls sys_tick_handler() every 1ms), also required for delay/millis
	systick_set_reload(120000);
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
	systick_counter_enable();
	systick_interrupt_enable();

	// TODO: load new options from VEXTREME/options.txt in key=val format
	sys_opt.size = sizeof(sys_opt);
	sys_opt.ver = 1;
	sys_opt.hw_ver = 0x0014; // v0.20
	sys_opt.sw_ver = 0x0017; // v0.23
	sys_opt.rgb_type = RGB_TYPE_10;

	xprintf("\nInited.\n");

	// HW version < 0.30
	if (sys_opt.hw_ver < 0x001e) {
		xprintf("HW VER < 0.30\n");
		if (sys_opt.rgb_type == RGB_TYPE_10) {
			xprintf("RGB_TYPE_10\n");
			ledsInitSW(10, GPIOB, GPIO14, GPIOB, GPIO13, RGB_BGR);
			ledsSetBrightness(150); // be careful not to set this too high when using white, those LEDs draw some power!!
		} else if (sys_opt.rgb_type == RGB_TYPE_4) {
			xprintf("RGB_TYPE_4\n");
			ledsInitSW(10, GPIOB, GPIO14, GPIOB, GPIO13, RGB_BGR);
			ledsSetBrightness(255); // we will be limiting to 4, it's ok to crank them all of the way up!
		} else if (sys_opt.rgb_type == RGB_TYPE_NONE) {
			xprintf("RGB_TYPE_NONE\n");
			ledsInitSW(10, GPIOB, GPIO14, GPIOB, GPIO13, RGB_BGR);
			ledsSetBrightness(0); // sleeper cart, you'll never see it coming _._
		}
	}
	// HW version >= 0.30
	else if (sys_opt.hw_ver >= 0x001e) { // >= 0.30
		xprintf("HW VER >= 0.30\n");
		ledsInitSW(4, GPIOB, GPIO14, GPIOB, GPIO13, RGB_BGR);
		ledsSetBrightness(255); // we will be limiting to 4, it's ok to crank them all of the way up!
	}

	// uint32_t start = millis();
	// while (millis() - start <= 2000UL) {
		// rainbowCycle(10);
		rainbowStep(4);
	// }

#if 0 // TEST LED CODE START
	while (1) {
		// color wipe back and fourth through the list of colors
		ledsClear();
		ledsSetBrightness(150);
		bool dir = true;
		for (int x = 1; x < sizeof(colors)/sizeof(*colors); x++) {
			colorWipe(dir, colors[x], 50);
			dir = !dir;
		}

		ledsClear();
		ledsSetBrightness(255);
		rainbowCycle(10);
		rainbowCycle(10);

		ledsClear();
		knightRider(6, 64, 4, 3, 9, 0xFF7700); // Cycles, Speed, Width, First, Last, RGB Color (original orange-red)
		knightRider(3, 32, 4, 3, 9, 0xFF00FF); // Cycles, Speed, Width, First, Last, RGB Color (purple)
		knightRider(3, 32, 4, 3, 9, 0x0000FF); // Cycles, Speed, Width, First, Last, RGB Color (blue)
		knightRider(3, 32, 5, 3, 9, 0x00FF00); // Cycles, Speed, Width, First, Last, RGB Color (green)
		knightRider(3, 32, 5, 3, 9, 0xFFFF00); // Cycles, Speed, Width, First, Last, RGB Color (yellow)
		knightRider(3, 32, 7, 3, 9, 0x00FFFF); // Cycles, Speed, Width, First, Last, RGB Color (cyan)
		knightRider(3, 32, 7, 3, 9, 0xFFFFFF); // Cycles, Speed, Width, First, Last, RGB Color (white)

		// Iterate through a whole rainbow of colors
		for(uint8_t j=0; j<252; j+=7) {
			knightRider(1, 16, 2, 0, 10, colorWheel(j)); // Cycles, Speed, Width, RGB Color
		}

		ledsClear();
		ledsSetBrightness(255);
		int y = 10;
		for (int x=0; x<10; x++) {
			theaterChaseRainbow(y);
			y += 5;
		}
	}
#endif // TEST LED CODE END

	//If USB power pin is high, boot into USB disk mode
	if (gpio_get(GPIOA, GPIO9)) {
		xprintf("USB dev mode.\n");
		ramdiskmain();
	} else {
		// disable this or it will delay our ROM emulation!
		systick_interrupt_disable();

		//Load the menu game
		strncpy(menuDir, "/roms", sizeof(menuDir));
		f_mount(&FatFs, "", 0);
		loadRom("/multicart.bin");
		loadListing(menuDir, &c_and_l.listing, menuIndex+1 , menuIndex+1+0x200, romData);

		//Go emulate a ROM.
		SYSCFG_MEMRMP=0x3; //mak ram at 0
		runptr=(char*)(((int)runptr&0x1ffff)|1); //map to lower mem
		xprintf("Gonna start romemu at %08x\n", romemu);
		runptr();
	}

	return 0;
}
