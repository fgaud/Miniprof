#include "miniprof.h"

static int msr_count;
static struct msr *available_msrs;

void cpuid(unsigned info, unsigned *eax, unsigned *ebx, unsigned *ecx, unsigned *edx) {
   *eax = info;
   __asm volatile
   ("mov %%ebx, %%edi;" 
    "cpuid;"
    "mov %%ebx, %%esi;"
    "mov %%edi, %%ebx;"
 : "+a"(*eax), "=S"(*ebx), "=c"(*ecx), "=d"(*edx)
       : : "edi");
}


unsigned int get_processor_family() {
   char vendor[12];
   unsigned int family;
   unsigned int a, b, c, d;

   cpuid(0x0, &a, (unsigned int *)vendor, (unsigned int *)(vendor + 8), (unsigned int *)(vendor + 4));
   if(memcmp(vendor, "AuthenticAMD", sizeof(vendor)))
      die("Unsupported CPU (expected AuthenticAMD, found %12.12s)\n", vendor);

   cpuid(0x1, &a, &b, &c, &d);
   family = (a & 0x0ff00f00);

   return family;
}

int can_be_used_10h(struct msr *msr, uint64_t evt) {
   return 1;
}

int can_be_used_15h(struct msr *msr, uint64_t evt) {
   if((evt & 0xe0) == 0xe0) {                              /* per node */
      return (msr->select & 0x40);
   } else {
      if(msr->select & 0x40)
         return 0;

      evt &= 0xF000000FF;
      if(evt <= 0x1F) {                                    /* FP       */
         if(evt == 0x0 || evt == 0x3 || evt == 0x4)
            return msr->id == 3;
         return msr->id >= 3;
      } else if(evt <= 0x3F) {                             /* LS       */
         if(evt == 0x23)
            return msr->id <= 2;
         return 1;
      } else if(evt <= 0x5F) {                             /* DC       */
         if(evt == 0x43 || evt == 0x45 || evt == 0x46)
            return msr->id <= 2;
         return 1;
      } else if(evt <= 0x7F) {                             /* CU       */
         return msr->id <= 2;
      } else if(evt <= 0x9F) {                             /* IC       */
         return msr->id <= 2;
      } else {                                             /* EX, DE   */
         if((evt >= 0xD0 && evt <= 0xD9) || evt == 0x1000000DD || evt == 0x1000000DE)
            return msr->id <= 2;
         if(evt == 0x1000000D0)
            return msr->id >= 3;
         return 1;
      }
   }
   return 0;
}

void get_available_msr(void) {
   if(available_msrs)
      return;

   int i;
   unsigned int family = get_processor_family();

   switch(family) {
   case 0x100f00: /* AMD fam10h, see AMD BKDG 10h, section 2.16.1 */
      msr_count = 4;
      available_msrs = malloc(msr_count * sizeof(*available_msrs));
      for(i = 0; i < msr_count; i++) {
         available_msrs[i].id = i;
         available_msrs[i].select =  0xC0010000 + i;
         available_msrs[i].value =  0xC0010000 + i + 4;
         available_msrs[i].can_be_used = can_be_used_10h;
         available_msrs[i].used = 0;
      }
      break;
   case 0x600f00: /* 15h */
      msr_count = 10;
      available_msrs = malloc(msr_count * sizeof(*available_msrs));
      for(i = 0; i < 6; i++) {
         available_msrs[i].id = i;
         available_msrs[i].select =  0xC0010200 + 2 * i;
         available_msrs[i].value =  0xC0010200 + 2 * i + 1;
         available_msrs[i].can_be_used = can_be_used_15h;
         available_msrs[i].used = 0;
      }
      for(i = 6; i < msr_count; i++) {
         available_msrs[i].id = i;
         available_msrs[i].select =  0xC0010240 + 2 * (i - 6);
         available_msrs[i].value =  0xC0010240 + 2 * (i - 6) + 1;
         available_msrs[i].can_be_used = can_be_used_15h;
         available_msrs[i].used = 0;
      }
      break;
   default:
      die("Unsupported processor family (%d)\n", family);
   }

}

/* Returns a MSR for a performance monitoring counter. */
struct msr *get_msr(uint64_t evt) {
   int i;

   get_available_msr();

   /* Perform search in reverse to increase the chance to use MSR 5-3 on 15h */
   /* because these counters can be used on a limited subset of events       */
   for(i = msr_count - 1; i >= 0; i--) {
      if(!available_msrs[i].used && available_msrs[i].can_be_used(&available_msrs[i], evt))
         return &available_msrs[i];
   }

   die("No free msr for event %llx", (long long unsigned)evt);
   return 0;
}

