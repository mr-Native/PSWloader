static unsigned reg_reads,reg_writes;
/* v1.96: optional ATOM_REGVALS=file ("dwordhex valuehex" per line, e.g. a PS5 Linux register dump) answers
 * reads with real values so control flow follows the real hardware state; unknown registers read zero. */
static uint32_t rv_reg[8192],rv_val[8192];static unsigned rv_n;static int rv_loaded;
static void rv_load(void){rv_loaded=1;const char*f=getenv("ATOM_REGVALS");if(!f)return;FILE*h=fopen(f,"r");if(!h)return;
 while(rv_n<8192&&fscanf(h,"%x %x",&rv_reg[rv_n],&rv_val[rv_n])==2)rv_n++;fclose(h);}
static uint32_t mock_read(struct card_info *c,uint32_t reg){(void)c;reg_reads++;if(!rv_loaded)rv_load();
 for(unsigned i=0;i<rv_n;i++)if(rv_reg[i]==reg){fprintf(stdout,"READ %x -> %x (dump)\n",reg,rv_val[i]);return rv_val[i];}
 fprintf(stdout,"READ %x -> zero (unknown)\n",reg);return 0;}
static void mock_write(struct card_info *c,uint32_t reg,uint32_t val){(void)c;reg_writes++;fprintf(stdout,"WRITE %x %x (MOCK ONLY)\n",reg,val);}
int main(int argc,char **argv){
 if(argc<3)return 2;static uint8_t rom[65536];FILE*f=fopen(argv[1],"rb");if(!f)return 2;size_t n=fread(rom,1,sizeof(rom),f);fclose(f);if(n!=65536)return 2;
 if(setjmp(host_abort)){fprintf(stdout,"STOP steps=%u reads=%u writes=%u\n",host_steps,reg_reads,reg_writes);return 1;}
 struct card_info card={0};card.reg_read=card.mc_read=card.pll_read=mock_read;card.reg_write=card.mc_write=card.pll_write=mock_write;
 struct atom_context c={0};c.card=&card;c.bios=rom;uint16_t base=get_u16(rom,0x48);c.cmd_table=get_u16(rom,base+30);c.data_table=get_u16(rom,base+32);
 static uint32_t scratch[512];c.scratch=scratch;c.scratch_size_bytes=sizeof(scratch);atom_index_iio(&c,get_u16(rom,c.data_table+0x32)+4);
 uint32_t params[64]={0};for(int i=3;i<argc&&i<67;i++)params[i-3]=(uint32_t)strtoul(argv[i],0,0);
 int status=amdgpu_atom_execute_table(&c,atoi(argv[2]),params,64);fprintf(stdout,"status=%d steps=%u reads=%u writes=%u; MOCK NOT HARDWARE\n",status,host_steps,reg_reads,reg_writes);free(c.iio);return status?1:0;
}
