// -----------------------------------------------------------------------------
// This file is part of fddEMU "Floppy Disk Drive Emulator"
// Copyright (C) 2021 Acemi Elektronikci
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
// -----------------------------------------------------------------------------


#include "fddEMU.h"
#include "FloppyDrive.h"
#include "pff.h"
#include "diskio.h"
#include "avrFlux.h"
#include "VirtualFloppyFS.h"
#include "simpleUART.h" //DEBUG
#include "UINotice.h" //msg.error

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <string.h> //for strcat,strcpy,...
#include <stdlib.h> //for itoa


//Global variables
bool pinsInitialized = false;
static struct floppySector sectorData;
volatile int8_t iTrack = 0;
volatile uint8_t iFlags = 0;

class FloppyDrive drive[N_DRIVE]; //will be used as extern

//Interrupt routines
#if defined (__AVR_ATmega328P__)
ISR(INT0_vect) //int0 pin 2 of port D
#elif defined (__AVR_ATmega32U4__)
ISR(INT2_vect) //int2
#endif //(__AVR_ATmega32U4__)
{
	if (IS_STEP() ) //debounce
		(STEPDIR()) ? --iTrack : ++iTrack;
	SET_TRACKCHANGED();
}

//Two drive mode requires SELECT and MOTOR pins combined trough an OR gate
//if two drive mode is enabled SELECTA pin is used for combined SELECTA & MOTORA
//and MOTORA pin is used for combined SELECTB & MOTORB
#if defined (__AVR_ATmega328P__)
ISR(PCINT2_vect) //pin change interrupt of port D
#elif defined (__AVR_ATmega32U4__)
ISR(PCINT0_vect) //pin change interrupt of port B
#endif //(__AVR_ATmega32U4__)
{
#if ENABLE_DRIVE_B
	if ( IS_SELECTA() ) SEL_DRIVE0(); //drive A is selected
	else if ( IS_SELECTB() ) SEL_DRIVE1(); //drive B is selected
#else //Drive B not enabled
	if ( IS_SELECTA() && IS_MOTORA() ) SEL_DRIVE0(); //driveA is selected
#endif  //ENABLE_DRIVE_B
	else CLR_DRVSEL();
}

void initFDDpins()
{
	//To emulate open collector outputs Output pins are set to LOW "0"
	//To write a "1" to output pin, respective pin is set to input
	//To write a "0" to output pin, respective pin is set to output
	pinsInitialized = false;
	cli(); //disable interrupts
#if defined (__AVR_ATmega328P__)
	//Setup Input and Output pins as Inputs
	DDRD &= 0b00000011; //D0 and D1 is RX & TX
	DDRB &= 0b11000000; //B6 & B7 is XTAL1 & XTAL2
	DDRC &= 0b11110000; //C7 is nil, C6 is RST, C4 & C5 is SDA & SCL
	//Assign Output pins LOW "0"
	PORTD &= ~(1 << PIN_INDEX);
	PORTB &= ~(1 << PIN_WRITEDATA);
	PORTC &= ~((1 << PIN_TRACK0)|(1 << PIN_WRITEPROT)|(1 << PIN_DSKCHANGE));
	//Assign Input pins HIGH "1" (Activate Pullups)
	PORTD |= (1 << PIN_MOTORA)|( 1 << PIN_SELECTA);
	PORTD |= (1 << PIN_STEP)|(1 << PIN_STEPDIR)|(1 << PIN_SIDE);
	PORTB |= (1 << PIN_READDATA);
	PORTC |= (1 << PIN_WRITEGATE);
	//Setup Pin Change Interrupts
	EICRA &=~((1 << ISC01)|(1 << ISC00)); //clear ISC00&ISC01 bits
	EICRA |= (1 << ISC01); //set ISC01 "falling edge"
	EIMSK |= (1 << INT0); //External Interrupt Mask Register enable INT0
	//Setup External Interrupt
	PCMSK2 = (1 << PIN_SELECTA)|(1 << PIN_MOTORA); // Pin Change Mask Register 2 enable SELECTA&MOTORA
	PCICR |= (1 << PCIE2); // Pin Change Interrupt Control Register enable port D
#elif defined (__AVR_ATmega32U4__)
	//Setup Input and Output pins as Inputs
	DDRB |= (1 << PIN_WRITEDATA); //set WRITEDATA as OUTPUT (Not sure it is necessary but datasheet says so)
	DDRB &= ~((1 << PIN_MOTORA)|(1 << PIN_SELECTA)); //PB0 RXLED, PB1 SCK, PB2 MOSI, PB3 MISO, PB4 MOTORA, PB5 OCP1, PB6 SELECTA
	DDRC &= ~(1 << PIN_SIDE); //PC6 SIDE
	DDRD &= ~((1 << PIN_STEP)|(1 << PIN_STEPDIR)|(1 << PIN_READDATA)|(1 << PIN_INDEX)|(1 << PIN_WRITEPROT)); //PD0 SCL, PD1 SDA, PD5 TXLED, PD2 STEP, PD3 STEPDIR, PD4 ICP1, PD7 INDEX
	DDRE &= ~(1 << PIN_WRITEGATE); //PE6 WRITEGATE
	DDRF &= ~((1 << PIN_TRACK0)|(1 << PIN_DSKCHANGE)); //PF4 TRACK0, PF5 WRITEPROT, PF6 DISKCHANGE, PF7 SS
	//Assign Output pins LOW "0"
	PORTB &= ~(1 << PIN_WRITEDATA);
	PORTD &= ~(1 << PIN_INDEX)|(1 << PIN_WRITEPROT);
	PORTF &= ~((1 << PIN_TRACK0)|(1 << PIN_DSKCHANGE));
	//Assign Input pins HIGH "1" (Activate Pullups)
	PORTB |= (1 << PIN_MOTORA)|(1 << PIN_SELECTA); //PB0 RXLED, PB1 SCK, PB2 MOSI, PB3 MISO, PB4 MOTORA, PB5 OCP1, PB6 SELECTA
	PORTC |= (1 << PIN_SIDE); //PC6 SIDE
	PORTD |= (1 << PIN_STEP)|(1 << PIN_STEPDIR)|(1 << PIN_READDATA); //PD0 SCL, PD1 SDA, PD5 TXLED, PD2 STEP, PD3 STEPDIR, PD4 ICP1, PD7 INDEX
	PORTE |= (1 << PIN_WRITEGATE); //PE6 WRITEGATE
	//Setup Pin Change Interrupts
	EICRA &=~(bit(ISC21)|bit(ISC20)); //clear ISC20&ISC21 bits
	EICRA |= bit(ISC21); //set ISC21 "falling edge"
	EIMSK |= bit(INT2); //External Interrupt Mask Register enable INT2
	//Setup External Interrupt
	PCMSK0 = bit(PIN_SELECTA)| bit(PIN_MOTORA); // Pin Change Mask Register 2 enable SELECTA&MOTORA
	PCICR |= bit(PCIE0); // Pin Change Interrupt Control Register enable port B
#endif //defined (__AVR_ATmega32U4__)
	pinsInitialized = true; //done
	sei(); //Turn interrupts on
}

void debugPrint_P(const __PGMSTR *debugStr)
{
#if DEBUG && SERIAL
	Serial.print(debugStr);
#endif //DEBUG && SERIAL
}

void debugPrintSector(char charRW)
{
#if DEBUG && SERIAL
	uint8_t track  = sectorData.header.track;
	uint8_t head   = sectorData.header.side;	
	uint8_t sector = sectorData.header.sector;

	Serial.write(charRW);
	Serial.printDEC(track);
	Serial.write('/');
	Serial.printDEC(head);
	Serial.write('/');
	Serial.printDEC(sector+1);
	Serial.write('\n');	

	if (charRW == 'W')
	{
		int8_t bytesPerLine = 16;
		int16_t startByte = ((sectorData.header.length == 1) && (sectorData.header.sector & 1 == 0)) ? 256:0;
		int16_t endByte = (sectorData.header.length == 1) ? startByte + 256:512;

		for (int i = startByte; i < endByte; i += bytesPerLine)
		{
			//write hex values
			for (j = 0; j < bytesPerLine; j++)
			{
				if (j > 0) Serial.write(' ');
				Serial.printHEX(pbuf[i+j]);				
			}
			Serial.write('\t');
			//write printable chars
			for (j = 0; j < bytesPerLine; j++)
			{
				char ch = pbuf[i+j];
				if (j > 0) Serial.write(' ');
				if (ch >= 32 && ch < 127) Serial.write(ch);
				else Serial.write('.');
			}
			Serial.write('\n');
		}
	}
#endif //DEBUG && SERIAL
}

FloppyDrive::FloppyDrive(void)
{
	if (!pinsInitialized) initFDDpins();
	bitLength = BIT_LENGTH_DD;	//To be more compatible: HD controllers support DD
	track = 0;
	side = 0;
	sector = 0;
}

char *FloppyDrive::diskInfoStr()	//Generate disk CHS info string
{
	static char infostring[12]; //drive C/H/S info string
	char convbuf[4];

	if (fName[0] == 0)
	{
		strcpy_P(infostring, str_nodisk);
		return infostring;
	}
	infostring[0] = 'C';
	infostring[1] = 0;
	itoa(numTrack, convbuf, 10); //max 255 -> 3 digits
	strcat(infostring, convbuf);
	strcat(infostring, "H2S");
	itoa(numSec, convbuf, 10); //max 255 -> 3 digits
	strcat(infostring, convbuf);
	return infostring;
}

int FloppyDrive::getSectorData(int lba)
{
	int n = FR_DISK_ERR;
	uint8_t *pbuf=sectorData.data;	

	if (isReady())
	{
		if (IS_HALFSECTOR())
			n = disk_read_sector(pbuf, startSector+(lba >> 1));
		else 
			n = disk_read_sector(pbuf, startSector+lba);
		if (n) msg.error(err_diskread);
	}
#if ENABLE_VFFS
	else if (isVirtual())
		n = vffs.readSector(pbuf, lba);
#endif //ENABLE_VFFS
	debugPrintSector('R');
	return n;
}

int FloppyDrive::setSectorData(int lba)
{
	int n = FR_DISK_ERR;
	uint8_t *pbuf=sectorData.data;
	uint16_t crc = calc_crc(&sectorData.id, 513);

	debugPrintSector('W');
	if ( (sectorData.crcHI == (crc >> 8)) || (sectorData.crcLO == (crc & 0xFF)) )
	{
		if (isReady())
		{
			if (IS_HALFSECTOR())
				n = disk_write_sector(pbuf, startSector+ (lba >> 1) );
			else	
				n = disk_write_sector(pbuf, startSector+lba);
			if (n) msg.error(err_diskwrite);
		}
	#if ENABLE_VFFS
		else if (isVirtual())
			n = vffs.writeSector(pbuf, lba);
	#endif //ENABLE_VFFS
	} 
	else debugPrint_P(F("CRC error!\n"));
	return n;
}

bool FloppyDrive::load(char *r_file)
{
	return (FloppyDisk::load(r_file));
}

void FloppyDrive::eject()
{
	FloppyDisk::eject();
	track = 0;
	side = 0;
	sector = 0;
}

void FloppyDrive::run()
{
	int16_t lba;
	int16_t res;
	int16_t crc;

	if (isChanged())
	{
		SET_DSKCHANGE_LOW();
		if (isReady() || isVirtual()) clrChanged();//if a disk is loaded clear diskChange flag
	}
	(isReadonly()) ? SET_WRITEPROT_LOW() : SET_WRITEPROT_HIGH();  //check readonly
	setup_timer1_for_write();
	while(GET_DRVSEL()) //PCINT for SELECTA and MOTORA
	{
		for (sector=0; (sector < numSec) && GET_DRVSEL(); sector++)
		{
		#if ENABLE_WDT
			wdt_reset();
		#endif  //ENABLE_WDT
		#if defined (__AVR_ATmega32U4__)
			Serial.rcvRdy(); //service usb
		#endif //defined (__AVR_ATmega32U4__)
			if (!GET_DRVSEL()) break; //if drive unselected exit loop
			side = (SIDE()) ? 0:1; //check side
			if (IS_TRACKCHANGED()) //if track changed
			{
				CLR_TRACKCHANGED();
				track += iTrack;	//add iTrack to current track
				iTrack = 0;				//reset iTrack
				if (track < 0) track=0; //Check if track valid
				else if (track >= numTrack) track = numTrack-1;
				(track == 0) ? SET_TRACK0_LOW() : SET_TRACK0_HIGH();
				isReady() || isVirtual() ? SET_DSKCHANGE_HIGH() : SET_DSKCHANGE_LOW(); //disk present ?
			}
			//start sector
			lba=(track*2+side)*numSec+sector;//LBA = (C × HPC + H) × SPT + (S − 1)
			getSectorData(lba); //get sector from SD
			setup_timer1_for_write();
			//prepare header
			sectorData.header.id = 0xFE; // ID mark
			sectorData.header.track = track;
			sectorData.header.side = side;
			sectorData.header.sector = sector + 1;
			sectorData.header.length = (IS_HALFSECTOR()) ? 1:2;
			crc = calc_crc((uint8_t*)&sectorData.header, 5);
			sectorData.header.crcHI = crc >> 8;
			sectorData.header.crcLO = crc & 0xFF;
      sectorData.header.gap = 0x4E;			
			//check for 256b sector
			if (IS_HALFSECTOR()) 
			{
				uint8_t *firstHalf = sectorData.data;
				uint8_t *secondHalf = firstHalf + 256;
				uint8_t *savedBytes = sectorData.extra;

				if ((sector & 1) == 0) //if first half
				{
					firstHalf[-1] = 0xFB;
					crc = calc_crc(firstHalf -1, 257);
					memcpy(savedBytes, secondHalf, 3); //save first 3 bytes of second half
					firstHalf[256] = crc >> 8;
					firstHalf[257] = crc & 0xFF;
					firstHalf[258] = 0x4E;
					res = write_sector(firstHalf -9, bitLength);
					if (res > 0) 
					{
						res = read_sector(firstHalf -9, bitLength);
						if (res) debugPrint_P(F("Read error!\n"));
						else setSectorData(lba); //save sector to SD
						while (IS_WRITE());//wait for WRITE_GATE to deassert
					}					
					memcpy(secondHalf, savedBytes, 3);
					crc = calc_crc(secondHalf -1, 257);
					secondHalf[256] = crc >> 8;
					secondHalf[257] = crc & 0xFF;
					secondHalf[258] = 0x4E;
					res = write_sector(secondHalf -9, bitLength);
					if (res > 0) 
					{
						res = read_sector(secondHalf -9, bitLength);
						if (res) debugPrint_P(F("Read error!\n"));
						else setSectorData(lba); //save sector to SD
						while (IS_WRITE());//wait for WRITE_GATE to deassert
					}
					memcpy(secondHalf -9, savedBytes, 9); //restore saved bytes 
				}
				while (IS_WRITE());//wait for WRITE_GATE to deassert
			}
			else //512b sector
			{
				//prepare sector
				sectorData.id = 0xFB;
				crc = calc_crc((uint8_t *)&sectorData.id, 513);
				sectorData.crcHI = crc >> 8;
				sectorData.crcLO = crc & 0xFF;
				sectorData.gap = 0x4E; 
				res = write_sector((uint8_t *)&sectorData, bitLength);
				if (res >  0) 
				{
					res = read_sector((uint8_t *)&sectorData, bitLength);
					if (res) debugPrint_P(F("Read error!\n"));
					else setSectorData(lba); //save sector to SD
					while (IS_WRITE());//wait for WRITE_GATE to deassert
				}
				else if (res < 0) debugPrint_P(F("Unsupported sector size!\n"));
			}			
		}//sectors
	}//selected
	SET_DSKCHANGE_HIGH();
	SET_WRITEPROT_HIGH();
}
