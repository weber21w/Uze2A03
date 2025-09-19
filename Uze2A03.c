#include "Uze2A03.h"


static void SilenceBuffer(){
	for(u16 i=0; i<262*2; i++)
		mix_buf[i] = 0x80;
}


static inline void spi_read_block(u32 addr, u8 *dst, u16 len){
	while(len){
		u8 bank = (u8)(addr>>16);
		u16 off = (u16)(addr&0xFFFF);
		u16 chunk = (u16)((0x10000u - off) < len ? (0x10000u - off) : len);
		SpiRamReadInto(bank, off, dst, chunk);
		addr += chunk; dst += chunk; len -= chunk;
	}
}


static inline void vgm_cache_init(u32 base_addr, u32 file_size){
	vgmC.base = base_addr;
	vgmC.size = file_size;
	vgmC.pos = vgmC.buf_start = vgmC.buf_len = 0;
}


static inline void vgm_seek(u32 abs_off){
	if(abs_off >= vgmC.size)
		abs_off = vgmC.size ? (vgmC.size-1) : 0;
	vgmC.pos = abs_off;
	vgmC.buf_len = 0;	//invalidate cache
	vstr.pos = abs_off;	//keep vstr in sync for EOF/loop math
}


static inline u8 vgm_getc(){	//1 byte, cached
	if(!(vgmC.pos >= vgmC.buf_start && vgmC.pos < (vgmC.buf_start + vgmC.buf_len))){
		vgmC.buf_start = vgmC.pos;
		u16 want = (u16)((vgmC.size - vgmC.buf_start) > VGM_CACHE_BYTES ? VGM_CACHE_BYTES : (vgmC.size - vgmC.buf_start));
		if(want == 0)
			want = 1;
		spi_read_block(vgmC.base + vgmC.buf_start, vgmC.buf, want);
		vgmC.buf_len = want;
	}
	u8 b = vgmC.buf[vgmC.pos - vgmC.buf_start];
	vgmC.pos++; vstr.pos = vgmC.pos;
	return b;
}


static inline u16 vgm_get16le(){
	u16 lo = vgm_getc();
	return (u16)(lo | ((u16)vgm_getc()<<8));
}


static inline u32 vgm_get32le(){
	u32 b0=vgm_getc(), b1=vgm_getc(), b2=vgm_getc(), b3=vgm_getc();
	return b0 | (b1<<8) | (b2<<16) | (b3<<24);
}


static inline void vgm_skip(u32 n){
	u32 p = vgmC.pos + n;
	vgm_seek(p);
}


static inline void dmc_cache_reset(u32 payload_off, u32 size){
	dmcC.base = vstr.base + payload_off;
	dmcC.size = size;
	dmcC.mask = ((size & (size-1))==0) ? (size-1) : 0;
	dmcC.seek = 0;
	dmcC.buf_start = dmcC.buf_len = 0;
}


static inline u32 dmc_wrap(u32 idx){
	if(!dmcC.size)
		return 0;
	if(dmcC.mask)
		return (idx & dmcC.mask);
	while(idx >= dmcC.size)
		idx -= dmcC.size;
	return idx;
}


static inline void dmc_fill_at(u32 start_idx){
	dmcC.buf_start = start_idx;
	u16 want = DMC_CACHE_BYTES;
	if(dmcC.size && (start_idx + want) > dmcC.size){
		u16 part1 = (u16)(dmcC.size - start_idx);
		u16 part2 = (u16)(want - part1);
		spi_read_block(dmcC.base + start_idx, dmcC.buf, part1);
		spi_read_block(dmcC.base + 0, dmcC.buf + part1, (part2 <= dmcC.size ? part2 : (u16)dmcC.size));
		dmcC.buf_len = want;
	}else{
		spi_read_block(dmcC.base + start_idx, dmcC.buf, want);
		dmcC.buf_len = want;
	}
}


static inline u8 dmc_read_rom(u16 addr){
	if(!dmcC.size)
		return 0x00;
	u32 idx = dmcC.seek + (u32)(((u32)addr - 0x8000u) & 0x7FFFu);
	idx = dmc_wrap(idx);

	if(!(idx >= dmcC.buf_start && idx < (dmcC.buf_start + dmcC.buf_len))){
		//u32 aligned = idx & (u32)(DMC_CACHE_BYTES-1 ? ~(DMC_CACHE_BYTES-1) : ~0u);
		u32 aligned = idx & ~(u32)(DMC_CACHE_BYTES - 1u);
		dmc_fill_at(aligned);
	}
	return dmcC.buf[idx - dmcC.buf_start];
}


static FRESULT load_vgm(u8 reload){
	u8 *as8 = (u8 *)accum_span;
	if(!reload)
		ptime_min = ptime_sec = ptime_frame = 0;
	SetRenderingParameters(33, 10*8);
	cony = 1;
	DrawWindow(1, 0, 28,8,NULL,NULL,NULL);
	SpiRamReadInto(0,0,as8,32);//get file to open(GUI places the selection at 0)


	FRESULT fr = pf_open((const char *)as8);
	if(fr)
		return fr;

	while(rcv_spi() != 0xFF);
	SD_DESELECT();

	u32 addr = VGM_BASE_ADDR;
	u8 bank = (u8)(addr >> 16);
	u16 off16 = (u16)(addr & 0xFFFF);

	UINT br;
	u8 buf[512];
	vgm_size = 0;
	
	while(1){
		fr = pf_read(buf, sizeof(buf), &br);
		while(rcv_spi() != 0xFF);
		SD_DESELECT();
		if(fr)
			return fr;
		if(br == 0)//EOF?
			break;

		SpiRamSeqWriteStart(bank, off16);//store the chunk
		for(u16 i=0; i<br; i++){
			SpiRamSeqWriteU8(buf[i]);
			if(++off16 == 0){//next bank?
				bank++;
			}
		}
		SpiRamSeqWriteEnd();
		vgm_size += br;
	}

	return FR_OK;
}


void Intro(){
	UMPrint(2,8,PSTR("Uze2A03 - NWCM Demo Edition"));
	FadeIn(4,1);
	WaitVsync(120);
	FadeOut(3,1);
	FadeIn(1,0);
}


int main(){
	SetFontTilesIndex(0);
	SetTileTable(tile_data);
	SetSpritesTileTable(tile_data);
	ClearVram();
	Intro();
	DrawWindow(1, 1, 28,10,NULL,NULL,NULL);
	do{
		WaitVsync(1);//let fade in finish before we load the skin..
	}while(DDRC != 255);
	LoadPreferences();
	//WORD	br;
	FRESULT res;
	u8 i;
	for(i=0; i<10 ; i++){
		res = pf_mount(&fs);
		if(!res){
			UMPrint(3,cony++,PSTR("Mounted SD Card"));
			i = 0;
			break;
		}
	}
	if(i){
		PrintByte(20,cony,res,0);
		UMPrint(3,cony++,PSTR("ERROR: SD Mount:"));
		goto MAIN_FAIL;
	}

	u8 bank_count = SpiRamInitGetSize();

	if(!bank_count){
		UMPrint(3,cony++,PSTR("ERROR: No SPI RAM detected"));
		goto MAIN_FAIL;
	}else{
		detected_ram = (u32)(bank_count*(64UL*1024UL));
		u8 moff = 21;		//>=65536
		if(bank_count > 1)	//>=131072
			moff++;
		if(bank_count > 15)	//>=1048576
			moff++;

		PrintLong(moff,cony,(u32)detected_ram/1024UL);
		UMPrintChar(moff+1,cony,'K');
		UMPrint(3,cony++,PSTR("SPI RAM Detected:"));
		WaitVsync(60);
	}

	SpiRamWriteStringEntryFlash(0, PSTR("Select File ^"));
	NextDir(NULL);
	SpiRamPrintString(4,0,(u32)(detected_ram-512),0,4);
	SetRenderingParameters(33,16);

	sprites[0].tileIndex = TILE_CURSOR;
	sprites[0].flags = sprites[0].x = sprites[0].y = 0;

	while(1){
		WaitVsync(1);
		update_2a03();
		//PrintInt(4,2,dmc_bytes_remaining,1);
	}
MAIN_FAIL:
	WaitVsync(240);
	SoftReset();
	return 0;
}


static void InputDeviceHandler(){//requires the kernel to *NOT* read controllers during VSYNC
	u8 i;
	joypad1_status_lo = joypad2_status_lo = joypad1_status_hi = joypad2_status_hi = 0;

	JOYPAD_OUT_PORT |= _BV(JOYPAD_LATCH_PIN);//latch controllers
	for(i=0; i<8+1; i++)//7 seems to glitch
		Wait200ns();
	JOYPAD_OUT_PORT&=~(_BV(JOYPAD_LATCH_PIN));//unlatch controllers

	for(i=0; i<16; i++){//read button states from the shift registers 
		joypad1_status_lo >>= 1;
		joypad2_status_lo >>= 1;

		JOYPAD_OUT_PORT &= ~(_BV(JOYPAD_CLOCK_PIN));//pulse clock pin

		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA1_PIN)) == 0)
			joypad1_status_lo |= (1<<15);
		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA2_PIN)) == 0)
			joypad2_status_lo |= (1<<15);
		
		JOYPAD_OUT_PORT |= _BV(JOYPAD_CLOCK_PIN);
		for(u8 j=0; j<33+1; j++)//32 seems to glitch?
			Wait200ns();
	}

	if(joypad1_status_lo == (BTN_START+BTN_SELECT+BTN_Y+BTN_B) || joypad2_status_lo == (BTN_START+BTN_SELECT+BTN_Y+BTN_B))
		SoftReset();
	
	for(i=0; i<8+1; i++)//wait 1.6us, any less and it glitches
		Wait200ns();

	for(i=0; i<16; i++){//Read extended mouse data on both ports(it's fine if there is no mouse there)
		joypad1_status_hi <<= 1;
		joypad2_status_hi <<= 1;
		JOYPAD_OUT_PORT &= ~(_BV(JOYPAD_CLOCK_PIN));//pulse clock pin(no delay required on Hyperkin)

		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA1_PIN)) == 0)
			joypad1_status_hi |= 1;
		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA2_PIN)) == 0)
			joypad2_status_hi |= 1;

		JOYPAD_OUT_PORT |= _BV(JOYPAD_CLOCK_PIN);
		for(u8 j=0; j<33+1; j++)//32 seems to glitch?
			Wait200ns();
	}
}


static void DrawWindow(u8 x, u8 y, u8 w, u8 h, const char *title, const char *lb, const char *rb){
	SetTile(x+0,y+0,TILE_WIN_TLC);
	SetTile(x+w,y+0,TILE_WIN_TRC);
	SetTile(x+0,y+h,TILE_WIN_BLC);
	SetTile(x+w,y+h,TILE_WIN_BRC);
	for(u8 y2=y+1; y2<y+h; y2++){
		for(u8 x2=x+1; x2<(x+w); x2++){
			SetTile(x2, y2, 0);
		}
	}
	for(u8 x2=x+1; x2<x+w; x2++){
		SetTile(x2,y,TILE_WIN_TBAR);
		SetTile(x2,y+h,TILE_WIN_BBAR);
	}
	for(u8 y2=y+1; y2<y+h; y2++){
		SetTile(x,y2,TILE_WIN_LBAR);
		SetTile(x+w,y2,TILE_WIN_RBAR);
	}
	if(title != NULL)
		UMPrint(x+1,y,title);
	if(lb != NULL)
		UMPrint(x+1,y+h,lb);
	if(rb != NULL){
		u8 xo = x+w;
		for(u8 i=0; i<16; i++){
			if(pgm_read_byte(&rb[i]) == '\0')
				break;
			xo--;
		}
		UMPrint(xo,y+h,rb);
	}
}


static void UpdateCursor(u8 ylimit){
	InputDeviceHandler();
	oldpad = pad;
	pad = ReadJoypad(0);
	u8 speed;
	if(pad & BTN_SR)
		speed = 1;
	else
		speed = 2;

	if((pad & BTN_LEFT)){
		if(sprites[0].x < speed)
			sprites[0].x = 0;
		else
			sprites[0].x -= speed;
	}else if((pad & BTN_RIGHT)){
		if(sprites[0].x+speed > (SCREEN_TILES_H*TILE_WIDTH)-9)
			sprites[0].x = (SCREEN_TILES_H*TILE_WIDTH)-9;
		else
			sprites[0].x += speed;
	}

	if((pad & BTN_UP)){
		if(sprites[0].y < speed)
			sprites[0].y = 0;
		else
			sprites[0].y -= speed;
	}else if((pad & BTN_DOWN)){
		if(sprites[0].y+8+speed > ylimit+3)
			sprites[0].y = ylimit-8+3;
		else
			sprites[0].y += speed;
	}

	for(u8 i=0; i<2; i++){
		u16 p = ReadJoypad(i);
		if(!(p & MOUSE_SIGNATURE))
			continue;

		u8 xsign,ysign;
		u16 deltax,deltay;
		if(i == 0){
			deltax = (joypad1_status_hi & 0b0000000001111111);
			deltay = (joypad1_status_hi & 0b0111111100000000)>>8;
			xsign = (joypad1_status_hi & 0b1000000000000000)?1:0;
			ysign = (joypad1_status_hi & 0b0000000010000000)?1:0;
		}else{
			deltax = (joypad2_status_hi & 0b0000000001111111);
			deltay = (joypad2_status_hi & 0b0111111100000000)>>8;
			xsign = (joypad2_status_hi & 0b1000000000000000)?1:0;
			ysign = (joypad2_status_hi & 0b0000000010000000)?1:0;
		}
		if(xsign){//right mouse movement
			deltax = -deltax;
			(u16)(deltax += sprites[0].x);
			(u16)(sprites[0].x = (deltax < 0)?0:deltax);
		}else{//left mouse movement
			(u16)(deltax += sprites[0].x);
			(u16)(sprites[0].x = (deltax > (SCREEN_TILES_H*TILE_WIDTH)-8)?(SCREEN_TILES_H*TILE_WIDTH)-8:deltax);
		}
		if(ysign){//up mouse movement
			deltay = -deltay;
			(u16)(deltay += sprites[0].y);
			(u16)(sprites[0].y = (deltay < 0)?0:deltay);
		}else{//down mouse movement
			(u16)(deltay += sprites[0].y);
			(u16)(sprites[0].y = (deltay > ylimit-4)?(ylimit-4):deltay);
		}
	}
}


static void PlayerInterface(){
	UpdateCursor(24);

	u8 btn,newclick=0;
	if(sprites[0].y >= (CONT_BAR_Y+CONT_BTN_H) || sprites[0].x < CONT_BAR_X || sprites[0].x >= CONT_BAR_X+CONT_BAR_W)
		btn = 255;
	else
		btn = (sprites[0].x-CONT_BAR_X)/CONT_BTN_W;

	static u8 lastbtn = 255;
	if((pad & (BTN_Y|BTN_MOUSE_LEFT)) && !(oldpad & (BTN_Y|BTN_MOUSE_LEFT))){
		lastbtn = btn;
		newclick = 1;
	}

	if(lastbtn != 255){
		u8 moff = 2+(lastbtn*2);
		u8 xoff = (CONT_BAR_X/8)+(lastbtn*(CONT_BTN_W/8));
		if((pad & BTN_Y)){//still holding?	
			SetTile(xoff+0,(CONT_BAR_Y/8)+0,pgm_read_byte(&pressed_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+0,pgm_read_byte(&pressed_map[moff]));
			moff += (CONT_BAR_W/TILE_WIDTH)-1;
			SetTile(xoff+0,(CONT_BAR_Y/8)+1,pgm_read_byte(&pressed_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+1,pgm_read_byte(&pressed_map[moff]));
		}else{//released
			SetTile(xoff+0,(CONT_BAR_Y/8)+0,pgm_read_byte(&buttons_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+0,pgm_read_byte(&buttons_map[moff]));
			moff += (CONT_BAR_W/TILE_WIDTH)-1;
			SetTile(xoff+0,(CONT_BAR_Y/8)+1,pgm_read_byte(&buttons_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+1,pgm_read_byte(&buttons_map[moff]));
			lastbtn = 255;
		}
	}
	ff_mul = 1;
	if(newclick){
		if(play_state & PS_LOADED){
			if(btn == 0){//previous

			}else if(btn == 1){//restart
				
			}else if(btn == 2){//pause
				if(play_state & PS_PAUSE)
					play_state ^= PS_PAUSE;
				else
					play_state = PS_LOADED|PS_PAUSE;
			}else if(btn == 3){//play
				play_state = PS_LOADED|PS_DRAWN|PS_PLAYING;
			}else if(btn == 4){//fast forward
				play_state = PS_LOADED|PS_DRAWN|PS_PLAYING;//eliminate pause if present
			}else if(btn == 5){//next

			}
		}
		if(btn == 6){
			WaitVsync(1);
			FileSelectWindow();
			pad = oldpad = 0b0000111111111111;
			WaitVsync(1);
			play_state &= ~PS_DRAWN;//force redraw
		}else if(btn == 7){//blank

		}else if(btn == 8){//volume down
			if(masterVolume)
				masterVolume--;
		}else if(btn == 9){//volume up
			masterVolume++;
			if(masterVolume > 64)
				masterVolume = 64;
		}else if(btn == 10){//skin prev
			DDRC = DDRC-1;
			u8 bad = 1;
			while(bad){
				bad = 0;
				for(u8 i=0; i<sizeof(bad_masks); i++){
					if(DDRC == pgm_read_byte(&bad_masks[i])){
						bad = 1;
						DDRC = DDRC-1;
						break;
					}
				}
			}
		}else if(btn == 11){//skin next
			DDRC = DDRC+1;
			u8 bad = 1;
			while(bad){
				bad = 0;
				for(u8 i=0; i<sizeof(bad_masks); i++){
					if(DDRC == pgm_read_byte(&bad_masks[i])){
						bad = 1;
						DDRC = DDRC+1;
						break;
					}
				}
			}
		}else if(btn == 12){//save pref
			SavePreferences();
		}
	}
	if(lastbtn != 255){
		if(lastbtn == 1){//fast backward
			//fast_backward = 2;
		}else if(lastbtn == 4){//fast forward
			ff_mul = 2;
		}
	}
}


static void LoadPreferences(){
	struct EepromBlockStruct ebs;
	ebs.id = UZENET_EEPROM_ID1;//use Uzenet color preference
	if(EepromReadBlock(ebs.id, &ebs) == 0){
		DDRC = ebs.data[29];
		//masterVolume = ebs.data[28];
		//if(masterVolume < 16 || masterVolume > 64)
		//masterVolume = 64;
	}else{
		DDRC = DEFAULT_COLOR_MASK;
		masterVolume = 64;
	}
}


static void SavePreferences(){
	struct EepromBlockStruct ebs;
	ebs.id = UZENET_EEPROM_ID1;

	if(EepromReadBlock(ebs.id, &ebs)){//doesn't exist, try to make it
		for(u8 i=0;i<30;i++)
			ebs.data[i] = 0;
		//ebs.data[28] = 64;//default master volume
		ebs.data[29] = DDRC;
		EepromWriteBlock(&ebs);
	}

	if(EepromReadBlock(ebs.id, &ebs) == 0){
		ebs.data[29] = DDRC;
		EepromWriteBlock(&ebs);
	}
}


static void UMPrintChar(u8 x, u8 y, char c){
	if(c >= 'a')
		c -= 32;
	SetTile(x,y,(c-32));
}


static void UMPrint(u8 x, u8 y, const char *s){
	u8 soff = 0;
	do{
		char c = pgm_read_byte(&s[soff++]);
		if(c == '\0')
			break;
		//if(c >= 'a')
		//	c -= 32;
		UMPrintChar(x++,y,c);
	}while(1);
}


static void UMPrintRam(u8 x, u8 y, char *s){
	u8 soff = 0;
	do{
		char c = s[soff++];
		if(c == '\0')
			break;
		//if(c >= 'a')
		//	c -= 32;
		UMPrintChar(x++,y,c);
	}while(1);
}


static void PrintSongTitle(u8 x, u8 y, u8 len){
/*
	SpiRamSeqReadStart((u8)(MOD_BASE >> 16), (u16)(MOD_BASE & 0xFFFF));
	u8 i=0;
	for(i=0; i<len; i++){
		u8 c = SpiRamSeqReadU8();
		if(c == 0)
			break;
		UMPrintChar(x + i, y, c);
	}
	SpiRamSeqReadEnd();
	for(; i<len; i++){
		SetTile(x+i,y,0);
	}
*/
}


static u8 IsRootDir(){
	u32 base = (u32)(detected_ram-512)+1;//assumes first current dir character is always '/'
	u8 c = SpiRamReadU8((u8)(base>>16),(u16)(base&0xFFFF));
	return (c == 0)?1:0;
}


static void PreviousDir(){
	u32 base = (u32)(detected_ram-512);
	u16 slen = SpiRamStringLen(base);
	u32 last_slash = 0;
	SpiRamSeqReadStart((u8)(base>>16),(u16)(base&0xFFFF));
	for(u16 i=0; i<slen-1; i++){
		if(SpiRamSeqReadU8() == '/')
			last_slash = i;
	}
	SpiRamSeqReadEnd();
	if(last_slash == 0)
		last_slash = 1;
	base += (u32)last_slash;
	SpiRamWriteU8((u8)(base>>16),(u16)(base&0xFFFF),0);
}


static void NextDir(char *s){
	u32 base = (u32)(detected_ram-512);
	if(s == NULL){
		SpiRamWriteStringEntryFlash((u32)base, PSTR("/"));
		return;
	}
	base += (u32)SpiRamStringLen((u32)base);
	u8 isroot = IsRootDir();
	SpiRamSeqWriteStart((u8)(base>>16),(u16)(base&0xFFFF));
	if(!isroot)
		SpiRamSeqWriteU8('/');
	while(1){
		u8 c = s[0];
		s++;
		if(c == 0)
			break;
		SpiRamSeqWriteU8(c);
	}
	//SpiRamSeqWriteU8('/');
	SpiRamSeqWriteEnd();
}


static u8 LoadDirData(u8 entry){

	total_files = 0;
	u8 *as8 = (u8 *)accum_span;

	SpiRamReadInto(0,0,as8,64);//save last loaded song, if any
	u32 base = (u32)(detected_ram-64);
	SpiRamWriteFrom((u8)(base>>16),(u16)(base&0xFFFF),as8,64);

	base = (u32)(entry*32UL);
	SpiRamReadInto((u8)(base>>16),(u16)(base&0xFFFF),as8,32);//load selected filename from GUI
	base = (u32)(detected_ram-512);
	SpiRamReadInto((u8)(base>>16),(u16)(base&0xFFFF),as8,256);//load current directory
	FRESULT res;
	FILINFO fno;
	DIR dir;
	res = pf_opendir(&dir, (const char *)as8);
	if(res == FR_OK){
		while(1){
			res = pf_readdir(&dir, &fno);
			if(res != FR_OK || fno.fname[0] == 0)
				break;

			if((fno.fattrib & AM_DIR)){//directory?
				SpiRamWriteStringEntry((u32)(total_files*32), '/', fno.fname);
				total_files++;
			}else{//file, make sure it's a VMG
				u8 valid = 0;
				for(u8 i=0; i<13-3; i++){
					if(fno.fname[i+0] == 0)
						break;
					if((fno.fname[i+0] == '.') && (fno.fname[i+1] == 'V') && (fno.fname[i+2] == 'G') && ((fno.fname[i+3] == 'M') || (fno.fname[i+3] == 'Z'))){
						valid = 1;
						break;
					}
				}
				if(valid){
					SpiRamWriteStringEntry((u32)(total_files*32), 0, fno.fname);
					total_files++;
				}
			}
		}
		dirty_sectors = 1+((total_files*64)/512);
		return 0;
	}
	dirty_sectors = 1+((total_files*64)/512);
	return res;
}


static void SpiRamWriteStringEntryFlash(u32 pos, const char *s){
	u8 i;
	SpiRamSeqWriteStart((u8)(pos>>16),(u16)(pos&0xFFFF));//file first entry so we can load the directory
	for(i=0; i<32; i++){
		char c = pgm_read_byte(s++);
		SpiRamSeqWriteU8(c);
		if(c == '\0')
			break;
	}
	for(; i<32; i++)
		SpiRamSeqWriteU8('\0');

	SpiRamSeqWriteEnd();
}


static void SpiRamWriteStringEntry(u32 pos, char prefix, char *s){
	u8 i;
	SpiRamSeqWriteStart((u8)(pos>>16),(u16)(pos&0xFFFF));//file first entry so we can load the directory
	if(prefix)
		SpiRamSeqWriteU8(prefix);
	for(i=0; i<32; i++){
		char c = *s++;
		SpiRamSeqWriteU8(c);
		if(c == '\0')
			break;
	}
	for(; i<32; i++)
		SpiRamSeqWriteU8('\0');

	SpiRamSeqWriteEnd();
}


static u16 SpiRamStringLen(u32 pos){
	SpiRamSeqReadStart((u8)(pos>>16),(u16)(pos&0xFFFF));
	u16 len = 0;
	while(SpiRamSeqReadU8() != 0 && len < 4096)
		len++;
	SpiRamSeqReadEnd();
	return len;
}


static u8 SpiRamPrintString(u8 x, u8 y, u32 pos, u8 invert, u8 fill){
	//u16 voff = (VRAM_TILES_H*y)+x;
	SpiRamSeqReadStart((u8)(pos>>16),(u16)(pos&0xFFFF));
	u8 ret = 255;
	while(1){
		char c = SpiRamSeqReadU8();
		if(ret == 255){
			ret = (c == '/')?1:0;//is directory?
		}
		if(c == '\0'){
			while(fill){
				SetTile(x++,y,0);
				fill--;
			}
			break;
		}
		if(invert)
			c += 64;
		if(fill)
			fill--;
		UMPrintChar(x++,y,c);
	}
	SpiRamSeqReadEnd();
	return ret;
}


static u8 ButtonHit(u8 x, u8 y, u8 w, u8 h){
	if(sprites[0].x < (x<<3) || sprites[0].x > (x<<3)+(w<<3) || sprites[0].y < (y<<3) || sprites[0].y > (y<<3)+(h<<3))
		return 0;
	return 1;
}


static void SpiRamCopyStringNoBuffer(u32 dst, u32 src, u8 max){
	while(max--){
		char c = SpiRamReadU8(0, (u16)(src&0xFFFF));
		SpiRamWriteU8((u8)(dst>>16), (u16)(dst&0xFFFF), c);
		if(!c)
			return;
		src++;
		dst++;
	}
	dst--;
	SpiRamWriteU8(0, (u16)(dst&0xFFFF),'\0');
}


static void FileSelectWindow(){
	SilenceBuffer();
	ClearVram();
	SetRenderingParameters(33, SCREEN_TILES_V*TILE_HEIGHT);

	u8 layer = 1;
	u8 loaded_dir = 0;
	u8 notroot = 0;
	u16 foff = 0;
	u8 lastclick = 255;
	u8 last_line = 255;
	while(1){
		WaitVsync(1);
		UpdateCursor(SCREEN_TILES_V*TILE_HEIGHT);
		u8 line = sprites[0].y/8;
		u8 click = 0;
		if(lastclick < 20)
			lastclick++;
		if(pad & (BTN_Y|BTN_SL|BTN_SR|BTN_MOUSE_LEFT) && !(oldpad & (BTN_Y|BTN_SL|BTN_SR|BTN_MOUSE_LEFT)))
			click = 1;

		if(layer == 0){
			DrawWindow(8,2,14,5,PSTR("Open File"),PSTR("Cancel"),NULL);
			UMPrint(10,4,PSTR("SD Card(L)"));
			UMPrint(10,5,PSTR("Network(R)"));
			if(line == 4 || line == 5){
				for(u8 i=9; i<8+14; i++)
					vram[(line*VRAM_TILES_H)+i] += 64;
			}

			if(click){
				if(pad & (BTN_SL|BTN_SR)){
					layer = (pad&BTN_SL)?1:2;
				}else if(line == 7 && sprites[0].x > 9*8 && sprites[0].x < 15*8){//cancel
					goto FILE_SELECT_END;
				}else if(line == 4){//SD card
					layer = 1;
				}else if(line == 5){//network
					layer = 2;
				}
			}
		}else if(layer == 1){//SD load
			u8 *as8 = (u8 *)accum_span;
			DrawWindow(4,2,22,SCREEN_TILES_V-3,PSTR("Select File"),PSTR("Cancel"),NULL);//was -6
			u8 fline = 0;
			if(line < 4)
				fline = (foff)?foff+1:0;
			else if(line > 3 && line < 4+10 && (foff+(line-4)) < total_files)
				fline = (foff+(line-3));
			else
				fline = ((foff+10)>total_files)?total_files:foff+10;
			PrintInt(16,SCREEN_TILES_V-1,fline,1);//was -4
			PrintInt(25,SCREEN_TILES_V-1,total_files,1);
			UMPrint(18,SCREEN_TILES_V-1,PSTR("of"));
			SetTile(26,2,TILE_WIN_SCRU);
			SetTile(26,SCREEN_TILES_V-1,TILE_WIN_SCRD);
			if(!loaded_dir){
				LoadDirData(0);
				loaded_dir = 1;
				notroot = !IsRootDir();
				UMPrint(0,0,PSTR("Dir: "));
				SpiRamPrintString(5,0,(u32)(detected_ram-512),0,SCREEN_TILES_H-6);//current path
			}
			UMPrint(5,3,notroot?PSTR(".."):PSTR("."));

			for(u16 i=0; i<10; i++){
				if(foff+i >= total_files)
					break;
				SpiRamPrintString(5,4+i,((foff+i)*32),0,0);
				if(line == i+4){
					for(u8 k=5; k<26; k++)
						vram[(line*VRAM_TILES_H)+k] += 64;
				}
			}
			if(last_line != line){//have we drawn the title for this file?
				last_line = line;
				if(line > 3 && line < 4+10 && (foff+(line-4)) < total_files){//valid filename area?
					u32 fbase = (u32)((foff+(line-4))*32);
					SpiRamReadInto((u8)(fbase>>16), (u16)(fbase&0xFFFF), as8, 32);
					if(as8[0] != '/'){//not a directory?
						FRESULT res;
						for(u8 i=0; i<3; i++){
							res =	pf_open((const char *)as8);
							if(!res){
								pf_lseek(0);
								break;
							}
							WaitVsync(30);
						}
						if(!res){
							WORD br;
							pf_read(as8,0x80,&br);
							//as8[20] = 0;//terminate string
							while(rcv_spi() != 0xFF);//wait for MISO idle
							UMPrint(0,1,PSTR("Title:"));
							UMPrintRam(7,1,(char *)as8);
							u8 slen = 0;
							for(u8 i=0; i<20; i++){
								if(as8[i] == 0)
									break;
								slen++;
							}
							for(u8 i=slen+7; i<20+7; i++)
								SetTile(i, 1, 0);
						}
					}else{
						UMPrint(0,1,PSTR("(Directory)"));
						for(u8 i=11; i<7+20; i++)
							SetTile(i,1,0);//blank over any remnants
					}
				}else
					for(u8 i=0; i<7+20; i++)
						SetTile(i,1,0);//blank any remnants
			}
			if(click){
				if(ButtonHit(4, SCREEN_TILES_V-1, 6, 1)){//cancel
					//SpiRamCopyStringNoBuffer(0, (u32)(detected_ram-64), 64);//restore previously playing filename, if any
					//if(SpiRamReadU8(0,0) != 0)
					//	PlayMOD(1);//force quick reload since lower memory is corrupted by GUI strings...
					//start_vgm(1);//start_nes(jumping,1);//HACK must fix any memory corrupted by file name data...
					goto FILE_SELECT_END;
				}else if(ButtonHit(26,2,1,1)){//up scroll
					if(foff < 10)
						foff = 0;
					else
						foff -= 10;
				}else if(ButtonHit(26,SCREEN_TILES_V-1,1,1)){//down scroll
					if(foff+10 <= total_files)
						foff += 10;
				}else if(ButtonHit(5,4,20,10)){//filename area?
					if(foff+(line-4) < total_files){//valid file?
						SpiRamCopyStringNoBuffer(0, (u32)((foff+(line-4))*32), 32);//copy this entry to first 32 bytes...
						char c = SpiRamReadU8(0, 0);
						if(c == '/'){//not a file, this is a directory to enter
							loaded_dir = 0;
							SpiRamReadInto(0,1,as8,64);//skip '/'
							NextDir((char *)as8);
						}else { //file, try to play it
	FRESULT fr = load_vgm(0);//load_vgm("TMNT.VGM");//load_vgm((char*)as8);
	if(fr == FR_OK && start_vgm(1) == 0){
		ff_mul	 = 1;
		play_state = PS_LOADED | PS_PLAYING;
	} else {
		NES_PLAYING = 0;
		SpiRamWriteU8(0,0,0);
		ClearVram();
		loaded_dir = 0;
	}
	goto FILE_SELECT_END;
}
						goto FILE_SELECT_END;
					}
				}else if(ButtonHit(5,3,20,1)){//previous directory?
					if(notroot){
						PreviousDir();
						loaded_dir = 0;
					}
				}
			}
		}else{//network load
			break;
		}
	}
FILE_SELECT_END:
	SpiRamCopyStringNoBuffer((u32)(detected_ram-64), 0, 64);//save currently playing file
	play_state &= ~PS_DRAWN;
	PrintSongTitle((CONT_BAR_X/8),(CONT_BAR_Y/8)+2,20);
	if(sprites[0].y > 20)
		sprites[0].y = 18;
	WaitVsync(1);
}

///////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////

static void stop_vgm(){
	write_reg(0x15, 0x00);//disable all channels and zero all length counters
	SilenceBuffer();
}


static u8 start_vgm(u8 looping){
	stop_vgm();
	reset_2a03();
	vgm_wait = vgm_samples_to_event = vgm_frac_44100 = 0;

	if(vgm_size < 0x40)
		return 2;
	vgm_stream_open(VGM_BASE_ADDR, vgm_size);
	if(ram_r32le(vstr.base + 0x00) != 0x206D6756UL)
		return 3;

	read_vgm_header();
	if(vstr.eof_off == 0 || vstr.eof_off > vstr.size)
		vstr.eof_off = vstr.size;
	if(vstr.data_off >= vstr.eof_off)
		return 4;
	if(vstr.loop_off && vstr.loop_off >= vstr.eof_off)
		vstr.loop_off = vstr.data_off;

	vgm_seek(vstr.data_off);
	NES_LOOPING = looping;
	NES_PLAYING = 1;

	while(vgm_wait == 0 && NES_PLAYING)
		parse_vgm();
	if(vgm_wait == 0 && NES_PLAYING)
		vgm_wait = 1;
	WaitVsync(1);
	return 0;
}


static inline u16 cpu_cycles_this_sample(){
	u16 cyc = (u16)CPU_PER_SAMPLE_INT;
	apu_frac += CPU_PER_SAMPLE_REM;
	if(apu_frac >= AUDIO_RATE_HZ){
		apu_frac -= AUDIO_RATE_HZ; cyc++;
	}
	return cyc;	//~113.853 cycles/sample
}


static inline void dmc_try_fill_sample_buffer(){
	if(!dmc_sample_buffer_empty || dmc_bytes_remaining == 0)
		return;

	dmc_sample_buffer = dmc_read_rom(dmc_cur_addr);
	dmc_sample_buffer_empty	= 0;
	if(++dmc_cur_addr == 0x0000)//wrap to 0x8000 after 0xFFFF(NES behavior)
		dmc_cur_addr = 0x8000;

	dmc_bytes_remaining--;
	if(dmc_bytes_remaining == 0 && dmc_loop){
		dmc_cur_addr = dmc_start_addr;
		dmc_bytes_remaining = dmc_start_len;
	}
}

#define P_GAIN		3	//squares gain
#define N_GAIN		2	//noise gain
#define T_GAIN		2	//triangle gain
#define D_GAIN		1	//DMC gain
#define HEADROOM_SHIFT	0	//extra attenuation: 0..2 (1 = /2)

static inline s8 clamp_s8(s16 v){
	if(v < -128)
		return -128;
	if(v > 127)
		return 127;
	return (s8)v;
}


static inline u8 mix_current_sample(){
	//center channels (remove DC only; no filter)
	s16 p1c = (s16)p1_output - (s16)(p1_volume >> 1);
	s16 p2c = (s16)p2_output - (s16)(p2_volume >> 1);
	s16 nzc = (s16)n_output - (s16)(n_volume >> 1);
	s16 trc = (s16)t_output - 8;	//0...15 -8....+7
	s16 dmc = (s16)dmc_output - 64;	//0..127 -64..+63

	s16 acc = 0;

	//Pulse mix
#if P_GAIN == 3
	acc += (p1c << 1) + p1c;	//*3
	acc += (p2c << 1) + p2c;	//*3
#elif P_GAIN == 2
	acc += (p1c << 1);		//*2
	acc += (p2c << 1);		//*2
#else
	acc += p1c * P_GAIN;
	acc += p2c * P_GAIN;
#endif

	//Noise mix
#if N_GAIN == 3
	acc += (nzc << 1) + nzc;	//*3
#elif N_GAIN == 2
	acc += (nzc << 1);		//*2
#else
	acc += nzc * N_GAIN;
#endif

	//Triangle mix
#if T_GAIN == 3
	acc += (trc << 1) + trc;	//*3
#elif T_GAIN == 2
	acc += (trc << 1);		//*2
#else
	acc += trc * T_GAIN;
#endif

	//DMC mix
#if D_GAIN == 3
	acc += (dmc << 1) + dmc;	//*3
#elif D_GAIN == 2
	acc += (dmc << 1);		//*2
#else
	acc += dmc * D_GAIN;		//*1
#endif

#if HEADROOM_SHIFT > 0
	acc >>= HEADROOM_SHIFT;
#endif

	return (u8)(0x80 + clamp_s8(acc));//return centered unsigned sample
}


static inline void step_square(volatile s16* t, u8 rlo, u8 rhi, u8* wave_idx, u16 dec, u8 en_mask){
	if((NES_REG[0x15] & en_mask)==0){//disabled?
		s16 nt = *t - dec;
		*t = (nt>0) ? nt:0;
		return;
	}
	s16 period = (s16)get_11_bit_timer(rlo,rhi)+1;
	s16 nt = *t - (s16)dec;
	if(nt > 0){
		*t = nt;
		return;
	}
	if(period <= 0){
		*t = 0;
		return;
	}

	u16 need = (u16)(-nt);
	if(need <= (u16)period){
		*t = (s16)(period - need);
		*wave_idx = (u8)((*wave_idx - 1) & 7);
	}else{
		u16 wraps = 1u + (u16)((need-1u)/(u16)period);
		*t = (s16)(wraps*period - need);
		*wave_idx = (u8)((*wave_idx - wraps) & 7);
	}
}


static inline u16 noise_period(){
	u8 idx = NES_REG[0x0E] & 0x0F;
	if(idx != n_idx_cached){
		n_idx_cached = idx;
		n_period_cached = pgm_read_word(&noise_table[idx]);
	}
	return n_period_cached;
}


static void apu_step_by(u16 cpu_cycles){
	//CPU to APU cycles(with 1-bit carry)
	static u8 carry = 0;
	u16 sum = (u16)(cpu_cycles + carry);
	carry = (u8)(sum & 1u);
	u16 apu_cycles = (u16)(sum >> 1);

	//Squares at APU rate
	step_square(&p1_11_bit_timer, 0x02,0x03, &p1_wave_index, apu_cycles, 0x01);
	step_square(&p2_11_bit_timer, 0x06,0x07, &p2_wave_index, apu_cycles, 0x02);

	//Noise at APU rate
	s16 nper = (s16)noise_period();
	s16 nt = n_timer - (s16)apu_cycles;
	if(nper > 0){
		while(nt <= 0){
			nt += nper;
			clock_lsfr();
		}
	}
	n_timer = (nt > 0) ? nt : 0;

	//Triangle @ APU rate (same clock as squares), period uses +1
	s16 tper = (s16)get_11_bit_timer(0x0A,0x0B) + 1;
	s16 tt = t_11_bit_timer - (s16)apu_cycles;
	if(tper > 0){
		while(tt <= 0){
			tt += tper;
			if(t_length_counter && t_linear_counter){
				t_wave_index = (t_wave_index ? t_wave_index-1 : 31);
			}
		}
	}
	t_11_bit_timer = (tt > 0) ? tt : 0;

	//DMC(bitrate in CPU cycles) ---
	if(dmc_enable){
		s16 dnt = (s16)dmc_timer - (s16)cpu_cycles;
		if(dnt > 0){
			dmc_timer = dnt;
		}else{
			u16 per = dmc_period ? dmc_period : 428;
			u16 need = (u16)(-dnt);

			u8 ticks = 1;
			if(need >= per){//compute ticks without division; always at least 1 when timer <= 0
				need -= per;
				ticks++;
				while(need >= per){
					need -= per;
					ticks++;
				}
			}
			dmc_timer = (s16)(per - need);

			do{
				if(dmc_bits_remaining == 0){
					if(!dmc_sample_buffer_empty){
						dmc_shift = dmc_sample_buffer;
						dmc_bits_remaining = 8;
						dmc_sample_buffer_empty = 1;
						dmc_try_fill_sample_buffer();//queue next fetch
					}
				}
				if(dmc_bits_remaining){
					if(dmc_shift & 1){
						if(dmc_output <= 125)
							dmc_output += 2;
					}else{
						if(dmc_output >= 2)
							dmc_output -= 2;
					}
					dmc_shift >>= 1;
					dmc_bits_remaining--;
				}
			}while(--ticks);
		}

		dmc_try_fill_sample_buffer();//keep sample buffer topped up when possible
	}

	fc_acc += cpu_cycles;//frame cunter
	const u8 five = (NES_REG[0x17] & 0x80) != 0;
	static const u16 d4[4] = {7457,7457,7458,7457};
	static const u16 d5[4] = {7457,7457,7458,14914};
	const u16 *d = five ? d5 : d4;
	while(fc_acc >= d[fc_phase]){
		fc_acc -= d[fc_phase];
		if(fc_phase==0 || fc_phase==2){
			clock_envelopes();
			clock_linear_counter();
		}else{
			clock_envelopes();
			clock_linear_counter();
			clock_length_counters();
			clock_sweep_units();
		}
		fc_phase = (fc_phase+1) & 3;
	}
}


static inline void vgm_stream_open(u32 base_addr, u32 file_size){
	vstr.base = base_addr;
	vstr.size = file_size;
	vstr.eof_off = file_size;	//will be overridden by header
	vstr.data_off = 0;
	vstr.loop_off = 0;
	vstr.pos = 0;
	vgm_cache_init(base_addr, file_size);
}


static inline u8 ram_r8(u32 a){
	return SpiRamReadU8((u8)(a>>16), (u16)(a&0xFFFF));
}


static inline u32 ram_r32le(u32 a){
	u32 b0=ram_r8(a+0), b1=ram_r8(a+1), b2=ram_r8(a+2), b3=ram_r8(a+3);
	return b0 | (b1<<8) | (b2<<16) | (b3<<24);
}


static inline u16 vgm_get_wait_as_audio_samples(){
	while(vgm_wait == 0){	//ensure parse_vgm() has produced a wait
		parse_vgm();	//applies immediate writes; keeps looping
		if(!NES_PLAYING)
			return 262;
	}

	//convert 44.1k VGM waits to AUDIO_RATE_HZ with carry
	//n = floor( (vgm_wait*AUDIO_RATE_HZ + vgm_frac) / 44100 )
	u32 num = vgm_frac_44100 + (u32)vgm_wait * (u32)AUDIO_RATE_HZ;
	u16 n = (u16)(num / 44100UL);
	vgm_frac_44100 = num % 44100UL;

	vgm_wait = 0;
	if(n == 0)
		n = 1;//ensure forward progress on tiny waits
	return n;
}


static void update_2a03(){
	PlayerInterface();
	if(!(play_state & PS_DRAWN)){
		ClearVram();
		DrawMap(5,0,buttons_map);
		PrintSongTitle((CONT_BAR_X/8),(CONT_BAR_Y/8)+2,20);
		play_state |= PS_DRAWN;
	}
	if((play_state & PS_PAUSE) || !(play_state & PS_PLAYING)){
		SilenceBuffer();
		goto DRAW_TIMER;
	}

	SetRenderingParameters(33, 1);
	if(++ptime_frame >= 50){
		ptime_frame = 0;
		if(++ptime_sec > 59){
			ptime_sec = 0;
			if(++ptime_min > 99)
				ptime_min = 99;
		}
	}

	u16 base = mix_bank ? 0u : 262u;

	if(!NES_PLAYING){
		for(u16 i=0; i<262u; i++) mix_buf[base+i] = 0x80;
		return;
	}

	u16 remain = 262;
	u8 *out = &mix_buf[base];

	while(remain){
	if(vgm_samples_to_event == 0)
		vgm_samples_to_event = vgm_get_wait_as_audio_samples();

	u16 chunk = (u16)((vgm_samples_to_event + ff_mul - 1) / ff_mul);
	if(chunk > remain)
	chunk = remain;

	for(u16 i = 0; i < chunk; i++){
		apu_step_by((u16)(ff_mul * cpu_cycles_this_sample()));
		sample_audio();
		*out++ = mix_current_sample();
	}

	u32 dec = (u32)chunk * ff_mul;
	vgm_samples_to_event = (dec >= vgm_samples_to_event) ? 0 : (u16)(vgm_samples_to_event - dec);

	remain -= chunk;
}

DRAW_TIMER:

	UMPrint(PTIME_X,PTIME_Y,PSTR("  :  :  "));
	PrintByte(PTIME_X+8,PTIME_Y,ptime_frame<<1,1);
	PrintByte(PTIME_X+5,PTIME_Y,ptime_sec,1);
	PrintByte(PTIME_X+2,PTIME_Y,ptime_min,0);
	UMPrintChar(PTIME_X+3,PTIME_Y,':');
	UMPrintChar(PTIME_X+6,PTIME_Y,':');
	SetRenderingParameters(33, 24);
}


static void parse_vgm(){
	if(vgm_wait){
		vgm_wait--;
		return;
	}

	if(vstr.pos >= vstr.eof_off){
		if(NES_LOOPING) vgm_seek(vstr.loop_off ? vstr.loop_off : vstr.data_off);
		else reset_2a03();
		return;
	}

	u8 cmd = vgm_getc();

	if(cmd == 0x61){
		vgm_wait = vgm_get16le();
		return;
	}
	if(cmd == 0x62){
		vgm_wait = 735;
		return;
	}
	if(cmd == 0x63){
		vgm_wait = 882;
		return;
	}
	if(cmd >= 0x70 && cmd <= 0x7F){
		vgm_wait = (cmd & 0x0F) + 1;
		return;
	}

	if(cmd == 0x66){
		if(NES_LOOPING)
			vgm_seek(vstr.loop_off ? vstr.loop_off : vstr.data_off);
		else
			reset_2a03();
		return;
	}

	if(cmd == 0x67){
		(void)vgm_getc();	// 0x66
		u8 type = vgm_getc();
		u32 size = vgm_get32le();
		u32 payload_off = vstr.pos;

		if((type & 0xF0) == 0xC0){
			dmc_cache_reset(payload_off, size);
		}
		vgm_seek(payload_off + size);
		return;
	}

	if(cmd == 0xE0){
		u32 ofs = vgm_get32le();
		if(dmcC.size){
			if(dmcC.mask)
				ofs &= dmcC.mask;
			else while(ofs >= dmcC.size)
				ofs -= dmcC.size;
			dmcC.seek = ofs;
		}
		return;
	}

	if(cmd == 0xB4){
		u8 reg = vgm_getc();
		u8 val = vgm_getc();
		if(reg < 0x18)
			write_reg(reg, val);
		return;
	}

	if(cmd == 0x4F || cmd == 0x50){
		vgm_skip(1);
		return;
	}
	if((cmd >= 0x51 && cmd <= 0x5F) || (cmd >= 0xA0 && cmd <= 0xAF) ||
		(cmd >= 0xB0 && cmd <= 0xB3) || (cmd >= 0xB5 && cmd <= 0xBF)){
		vgm_skip(2);
		return;
	}
	if(cmd >= 0xC0 && cmd <= 0xCF){
		vgm_skip(3);
		return;
	}
	return;
}


static void read_vgm_header(){
	//vstr.base already set by vgm_stream_open()
	VGM_EOF_OFFSET		= ram_r32le(vstr.base + 0x04) + 0x04;
	VGM_TOTAL_NUM_SAMPLES	= ram_r32le(vstr.base + 0x18);
	//VGM_RATE		= ram_r32le(vstr.base + 0x24);

	u32 data_ofs		= ram_r32le(vstr.base + 0x34);
	VGM_DATA_OFFSET		= data_ofs ? (data_ofs + 0x34) : 0x40;//spec fallback
	VGM_NES_APU_CLOCK	= ram_r32le(vstr.base + 0x84);

	VGM_LOOP_OFFSET		= ram_r32le(vstr.base + 0x1C);
	if(VGM_LOOP_OFFSET)
		VGM_LOOP_OFFSET += 0x1C;

	//stash into stream for quick jumps
	vstr.data_off = VGM_DATA_OFFSET;
	vstr.loop_off = VGM_LOOP_OFFSET;
	vstr.eof_off = VGM_EOF_OFFSET;
}


static void reset_2a03(){
	for(u8 i=0; i<24; i++){
		NES_REG[i] = 0b00000000;
	}

	//Pulse 1/2 variables
	p1_output = p2_output = 0;
	p1_11_bit_timer = p2_11_bit_timer = 0;
	p1_wave_index = p2_wave_index = 0;
	p1_length_counter = p2_length_counter = 0;
	p1_envelope_divider = p2_envelope_divider = 0;
	p1_decay_counter = p2_decay_counter = 0;
	p1_volume = p2_volume = 0;

	//Noise variables
	n_output = 0;
	n_timer = 0;
	n_length_counter = 0;
	n_envelope_divider = 0;
	n_decay_counter = 0;
	n_volume = 0;
	n_lsfr = 1;

	//Triangle Variables
	t_output = 0;
	t_11_bit_timer = 0;
	t_wave_index = 0;
	t_length_counter = 0;
	t_linear_counter = 0;
	t_lin_reload = 0;
	vgm_wait = 0;
	NES_PLAYING = 0;

	//DMC variables
	dmc_output = 64;
	dmc_period = 428;
	dmc_timer = dmc_enable = dmc_irq = dmc_loop = dmc_rate_idx = 0;
	dmc_cur_addr = dmc_start_addr = 0xC000;
	dmc_bytes_remaining = dmc_start_len = dmc_shift = dmc_bits_remaining = dmc_sample_buffer = 0;
	dmc_sample_buffer_empty = 1;
}


static void sample_audio(){
	//Pulse 1
	if(p1_length_counter > 0 && !p1_swp_mute && p1_11_bit_timer >= 8){
		p1_output = pgm_read_byte(&duty_table[get_duty(0x00)][p1_wave_index]) ? p1_volume : 0;
	}else{
		p1_output = 0;
	}

	//Pulse 2
	if(p2_length_counter > 0 && !p2_swp_mute && p2_11_bit_timer >= 8){
		p2_output = pgm_read_byte(&duty_table[get_duty(0x04)][p2_wave_index]) ? p2_volume : 0;
	}else{
		p2_output = 0;
	}

	if(t_length_counter > 0 && t_linear_counter > 0 && get_11_bit_timer(0x0A,0x0B) >= 2){
		t_output = pgm_read_byte(&tri_table[t_wave_index]); // 0..15
	}else{
		t_output = 0;
	}


	//Noise
	if(n_length_counter > 0){
		u8 noise_bit = (u8)((~n_lsfr) & 1);//1 when bit0==0, else 0
		n_output = (u16)n_volume * noise_bit;//0..n_volume
	}else{
		n_output = 0;
	}
}


static void clock_lsfr(){
	u8 tap = (NES_REG[0x0E] & 0x80) ? 6 : 1;	//mode: 1->tap=6, else tap=1
	u8 bit0 = (u8)(n_lsfr & 1);
	u8 bitT = (u8)((n_lsfr >> tap) & 1);
	u8 fb = (u8)(bit0 ^ bitT);
	n_lsfr >>= 1;
	n_lsfr |= (u16)(fb << 14);
}


static void write_reg(u8 reg, u8 val){
	NES_REG[reg] = val;//always store the raw write
	switch(reg){
		case 0x00: {	//$4000 P1 vol/env
			p1_decay_counter = 15;
			p1_envelope_divider = 0;
			break;
		}

		case 0x04: {	//$4004 P2 vol/env
			p2_decay_counter = 15;
			p2_envelope_divider = 0;
			break;
		}

		case 0x0C: {	//$400C Noise vol/env
			n_decay_counter = 15;
			n_envelope_divider = 0;
			break;
		}

		case 0x03: {	//$4003 P1 length load + timer high
			u8 level = (val & 0x08) ? 1 : 0;
			u8 index = val >> 4;
			p1_length_counter	= pgm_read_byte(&length_table[level][index]);
			p1_wave_index		= 0;
			p1_decay_counter	= 15;
			p1_envelope_divider	= 0;
			p1_swp_reload		= 1;
			break;
		}

		case 0x07: {	//$4007 P2 length load + timer high
			u8 level = (val & 0x08) ? 1 : 0;
			u8 index = val >> 4;
			p2_length_counter	= pgm_read_byte(&length_table[level][index]);
			p2_wave_index		= 0;
			p2_decay_counter	= 15;
			p2_envelope_divider	= 0;
			p2_swp_reload		= 1;
			break;
		}

		case 0x08: {	//$4008: triangle linear counter(ctrl/halt + reload value)
			t_lin_reload = 1;
			break;
		}

		case 0x17: {	//$4017 frame counter
			//bit7: 1 = 5-step, 0 = 4-step; bit6: IRQ inhibit
			fc_acc = 0;
			fc_phase = 0;

			if(NES_REG[0x17] & 0x80){//immediate clocks in 5-step mode
					clock_envelopes();
					clock_linear_counter();
					clock_length_counters();
					clock_sweep_units();
			}
			break;
		}

		case 0x0B: {	//$400B Triangle length load + timer high
			u8 level = (val & 0x08) ? 1 : 0;
			u8 index = val >> 4;
			t_length_counter = pgm_read_byte(&length_table[level][index]);
			t_wave_index	 = 0;
			t_lin_reload	 = 1;
			break;
		}

		case 0x0F: {	//$400F Noise length load
			u8 level = (val & 0x08) ? 1 : 0;
			u8 index = val >> 4;
			n_length_counter = pgm_read_byte(&length_table[level][index]);
			n_decay_counter	= 15;
			n_envelope_divider = 0;
			break;
		}

		case 0x10: {	//$4010: [7]=IRQ, [6]=loop, [3..0]=rate idx
			dmc_irq		= (val >> 7) & 1;
			dmc_loop	= (val >> 6) & 1;
			dmc_rate_idx	= (val & 0x0F);
			dmc_period = pgm_read_word(&dmc_rate_table[dmc_rate_idx]);
			break;
		}

		case 0x11: {	//$4011: direct DAC load (7-bit)
			dmc_output = (val & 0x7F);
			break;
		}

		case 0x12: {	//$4012: sample start address
			dmc_start_addr = (u16)(0xC000u + ((u16)val << 6));
			break;
		}

		case 0x13: {	//$4013: sample length
			dmc_start_len = (u16)(((u16)val << 4) + 1u);
			break;
		}
/*
		case 0x15: {	//$4015 enables
			if(!(val & 0x01))
				p1_length_counter = 0;
			if(!(val & 0x02))
				p2_length_counter = 0;
			if(!(val & 0x04))
				t_length_counter = 0;
			if(!(val & 0x08))
				n_length_counter = 0;
			if(val & 0x10){
				dmc_enable = 1;
				if(dmc_bytes_remaining == 0){
					dmc_cur_addr		= dmc_start_addr;
					dmc_bytes_remaining	= dmc_start_len;
				}
				dmc_bits_remaining = 0;		//force reload on next tick
				dmc_try_fill_sample_buffer();	//prime now
			}else{
				dmc_enable = 0;
				dmc_bytes_remaining = 0;	// DAC holds last level
			}
			break;
		}
*/

		case 0x15: {	//$4015 enables
			if(!(val & 0x01))
				p1_length_counter = 0;
			if(!(val & 0x02))
				p2_length_counter = 0;
			if(!(val & 0x04))
				t_length_counter = 0;
			if(!(val & 0x08))
				n_length_counter = 0;

			const u8 was_enabled = dmc_enable;
			const u8 now_enabled = (val & 0x10) ? 1 : 0;

			dmc_enable = now_enabled;
			dmc_irq = 0;

			if(now_enabled){
				//only (re)start on 0->1 transition or when inactive
				if(!was_enabled && dmc_bytes_remaining == 0){
					dmc_cur_addr = dmc_start_addr;	// $4012 base
					dmc_bytes_remaining = dmc_start_len;	// $4013 length
				}

				if(dmc_sample_buffer_empty && dmc_bytes_remaining){//matches HW: enabling triggers immediate fetch when buffer is empty
					dmc_try_fill_sample_buffer();
				}

			}else{
				dmc_bytes_remaining = 0;//intentionally not clearing dmc_bit_remaining or dmc_shift
			}
			break;
		}

		case 0x01:	//$4001 P1 sweep
			p1_swp_reload = 1;
			break;

		case 0x05:	//$4005 P2 sweep
			p2_swp_reload = 1;
			break;

		default:
			break;
	}
}


static void clock_envelopes(){
	//Pulse 1
	u8 p1_envelope_disable = NES_REG[0x00]&0b00010000;
	if(p1_envelope_disable){
		p1_volume = NES_REG[0x00]&0b00001111;
	}else{
		u8 p1_envelope_loop = NES_REG[0x00]&0b00100000;

		if(p1_envelope_divider > 0){
			p1_envelope_divider--;
		}else{
			p1_envelope_divider = NES_REG[0x00]&0b00001111;

			if(p1_decay_counter > 0){
				p1_decay_counter--;
			}else if(p1_envelope_loop == 1){
				p1_decay_counter = 15;
			}
		}
		p1_volume = p1_decay_counter;
	}

	//Pulse 2
	u8 p2_envelope_disable = NES_REG[0x04]&0b00010000;
	if(p2_envelope_disable){
		p2_volume = NES_REG[0x04]&0b00001111;
	}else{
		u8 p2_envelope_loop = NES_REG[0x04]&0b00100000;

		if(p2_envelope_divider > 0){
			p2_envelope_divider--;
		}else{
			p2_envelope_divider = NES_REG[0x04]&0b00001111;

			if(p2_decay_counter > 0){
				p2_decay_counter--;
			}else if(p2_envelope_loop){
				p2_decay_counter = 15;
			}
		}
		p2_volume = p2_decay_counter;
	}

	//Noise
	u8 n_envelope_disable = NES_REG[0x0C]&0b00010000;
	if(n_envelope_disable){
		n_volume = NES_REG[0x0C]&0b00001111;
	}else{
		u8 n_envelope_loop = NES_REG[0x0C]&0b00100000;

		if(n_envelope_divider){
			n_envelope_divider--;
		}else{
			u8 d = NES_REG[0x0C]&0b00001111;
			n_envelope_divider = d;

			if(n_decay_counter > 0){
				n_decay_counter--;
			}else if(n_envelope_loop == 1){
					n_decay_counter = 15;
			}
		}
		n_volume = n_decay_counter;
	}
}


static void clock_linear_counter(){
	u8 ctrl = NES_REG[0x08] & 0b10000000;//bit7 = control (halt)
	u8 reload_val = NES_REG[0x08] & 0b01111111;//$4008

	if(t_lin_reload){
		t_linear_counter = reload_val;
	}else if(t_linear_counter > 0){
		t_linear_counter--;
	}
	if(!ctrl)
		t_lin_reload = 0;
}


static void clock_length_counters(){
	u8 length_counter_halt_flag;

	//Pulse 1
	length_counter_halt_flag = NES_REG[0x00]&0b00100000;
	if(!length_counter_halt_flag){
		if(p1_length_counter > 0){
			p1_length_counter--;
		}
	}

	//Pulse 2
	length_counter_halt_flag = NES_REG[0x04]&0b00100000;
	if(!length_counter_halt_flag){
		if(p2_length_counter > 0){
			p2_length_counter--;
		}
	}

	//Noise
	length_counter_halt_flag = NES_REG[0x0C]&0b00100000;
	if(!length_counter_halt_flag){
		if(n_length_counter > 0){
			n_length_counter--;
		}
	}

	//Triangle
	length_counter_halt_flag = NES_REG[0x08]&0b10000000;
	if(!length_counter_halt_flag){
		if(t_length_counter > 0){
			t_length_counter--;
		}
	}
}


static inline u8 get_duty(u8 reg){
	return NES_REG[reg]>>6;
}


static u16 get_11_bit_timer(u8 reg_low, u8 reg_high){
	u8 lo = NES_REG[reg_low];
	u8 hi = NES_REG[reg_high] & 0x07; // keep low 3 bits
	return ((u16)hi << 8) | lo;
}


static inline void set_11_bit_timer(u8 reg_low, u8 reg_high, u16 val){
	u8 lo = (u8)(val & 0xFF);
	u8 hi = (u8)((NES_REG[reg_high] & 0xF8) | ((val >> 8) & 0x07));
	NES_REG[reg_low] = lo;
	NES_REG[reg_high] = hi;
}


static inline u16 sweep_target_for(u8 ch){	//ch=0: P1, ch=1: P2
	const u8 reg_swp = ch ? 0x05 : 0x01;	//$4005 or $4001
	const u8 reg_lo = ch ? 0x06 : 0x02;
	const u8 reg_hi = ch ? 0x07 : 0x03;

	u8 sw = NES_REG[reg_swp];
	u16 p = get_11_bit_timer(reg_lo, reg_hi)+1;
	u8 sh = (u8)(sw & 0x07);
	if(sh == 0)
		return p;

	u16 change = (u16)(p >> sh);
	if(sw & 0x08){	//negate
		//quirk: Pulse 1 does one’s-complement negate (extra -1)
		return (u16)(p - change - (ch ? 0 : 1));
	}else{
		return (u16)(p + change);
	}
}


static inline void sweep_tick_one(u8 ch){
	const u8 reg_swp = ch ? 0x05 : 0x01;	//$4005/$4001
	const u8 reg_lo = ch ? 0x06 : 0x02;	//$4006/$4002
	const u8 reg_hi = ch ? 0x07 : 0x03;	//$4007/$4003

	u8 sw = NES_REG[reg_swp];
	u8 en = (u8)((sw >> 7) & 1);
	u8 per = (u8)((sw >> 4) & 7);
	u8 sh = (u8)(sw & 7);

	u16 p = get_11_bit_timer(reg_lo, reg_hi);//+1?
	u16 targ = sweep_target_for(ch);

	u8 *div	= ch ? &p2_swp_div : &p1_swp_div;
	u8 *reload = ch ? &p2_swp_reload : &p1_swp_reload;
	u8 *mute = ch ? &p2_swp_mute : &p1_swp_mute;

	*mute = (p < 8) || (targ > 0x7FF);

	if(*div == 0){
		if(en && sh && !*mute){
			set_11_bit_timer(reg_lo, reg_hi, targ);
		}
		*div = per;
	}else if(*reload){
		*div = per;
		*reload = 0;
	}else{
		(*div)--;
	}
}


static void clock_sweep_units(){
	sweep_tick_one(0);
	sweep_tick_one(1);

}
