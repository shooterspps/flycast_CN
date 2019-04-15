#include "mmu.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h"
#include "types.h"

#ifdef FAST_MMU

#include "hw/mem/_vmem.h"

#include "mmu_impl.h"
#include "ccn.h"
#include "hw/sh4/sh4_mem.h"
#include "oslib/oslib.h"

extern TLB_Entry UTLB[64];
// Used when FullMMU is off
extern u32 sq_remap[64];

//#define TRACE_WINCE_SYSCALLS

#include "wince.h"

#define printf_mmu(...)
//#define printf_mmu printf
#define printf_win32(...)

extern const u32 mmu_mask[4];
extern const u32 fast_reg_lut[8];

const TLB_Entry *lru_entry = NULL;
static u32 lru_mask;
static u32 lru_address;

struct TLB_LinkedEntry {
	TLB_Entry entry;
	TLB_LinkedEntry *next_entry;
};
#define NBUCKETS 65536
TLB_LinkedEntry full_table[65536];
u32 full_table_size;
TLB_LinkedEntry *entry_buckets[NBUCKETS];

static u16 bucket_index(u32 address, int size)
{
	return ((address >> 16) ^ ((address & 0xFC00) | size)) & (NBUCKETS - 1);
}

static void cache_entry(const TLB_Entry &entry)
{
	verify(full_table_size < ARRAY_SIZE(full_table));
	u16 bucket = bucket_index(entry.Address.VPN << 10, entry.Data.SZ1 * 2 + entry.Data.SZ0);

	full_table[full_table_size].entry = entry;
	full_table[full_table_size].next_entry = entry_buckets[bucket];
	entry_buckets[bucket] = &full_table[full_table_size];
	full_table_size++;
}

static void flush_cache()
{
	full_table_size = 0;
	memset(entry_buckets, 0, sizeof(entry_buckets));
}

template<u32 size>
bool find_entry_by_page_size(u32 address, const TLB_Entry **ret_entry)
{
	u32 shift = size == 1 ? 2 :
			size == 2 ? 6 :
			size == 3 ? 10 : 0;
	u32 vpn = (address >> (10 + shift)) << shift;
	u16 bucket = bucket_index(vpn << 10, size);
	TLB_LinkedEntry *pEntry = entry_buckets[bucket];
	u32 length = 0;
	while (pEntry != NULL)
	{
		if (pEntry->entry.Address.VPN == vpn && (size >> 1) == pEntry->entry.Data.SZ1 && (size & 1) == pEntry->entry.Data.SZ0)
		{
			if (pEntry->entry.Data.SH == 1 || pEntry->entry.Address.ASID == CCN_PTEH.ASID)
			{
				*ret_entry = &pEntry->entry;
				return true;
			}
		}
		pEntry = pEntry->next_entry;
	}

	return false;
}

static bool find_entry(u32 address, const TLB_Entry **ret_entry)
{
	// 4k
	if (find_entry_by_page_size<1>(address, ret_entry))
		return true;
	// 64k
	if (find_entry_by_page_size<2>(address, ret_entry))
		return true;
	// 1m
	if (find_entry_by_page_size<3>(address, ret_entry))
		return true;
	// 1k
	if (find_entry_by_page_size<0>(address, ret_entry))
		return true;
	return false;
}

#if 0
static void dump_table()
{
	static int iter = 1;
	char filename[128];
	sprintf(filename, "mmutable%03d", iter++);
	FILE *f = fopen(filename, "wb");
	if (f == NULL)
		return;
	fwrite(full_table, sizeof(full_table[0]), full_table_size, f);
	fclose(f);
}

int main(int argc, char *argv[])
{
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL)
	{
		perror(argv[1]);
		return 1;
	}
	full_table_size = fread(full_table, sizeof(full_table[0]), ARRAY_SIZE(full_table), f);
	fclose(f);
	printf("Loaded %d entries\n", full_table_size);
	std::vector<u32> addrs;
	std::vector<u32> asids;
	for (int i = 0; i < full_table_size; i++)
	{
		u32 sz = full_table[i].entry.Data.SZ1 * 2 + full_table[i].entry.Data.SZ0;
		u32 mask = sz == 3 ? 1*1024*1024 : sz == 2 ? 64*1024 : sz == 1 ? 4*1024 : 1024;
		mask--;
		addrs.push_back(((full_table[i].entry.Address.VPN << 10) & mmu_mask[sz]) | (random() * mask / RAND_MAX));
		asids.push_back(full_table[i].entry.Address.ASID);
//		printf("%08x -> %08x sz %d ASID %d SH %d\n", full_table[i].entry.Address.VPN << 10, full_table[i].entry.Data.PPN << 10,
//				full_table[i].entry.Data.SZ1 * 2 + full_table[i].entry.Data.SZ0,
//				full_table[i].entry.Address.ASID, full_table[i].entry.Data.SH);
		u16 bucket = bucket_index(full_table[i].entry.Address.VPN << 10, full_table[i].entry.Data.SZ1 * 2 + full_table[i].entry.Data.SZ0);
		full_table[i].next_entry = entry_buckets[bucket];
		entry_buckets[bucket] = &full_table[i];
	}
	for (int i = 0; i < full_table_size / 10; i++)
	{
		addrs.push_back(random());
		asids.push_back(666);
	}
	double start = os_GetSeconds();
	int success = 0;
	const int loops = 100000;
	for (int i = 0; i < loops; i++)
	{
		for (int j = 0; j < addrs.size(); j++)
		{
			u32 addr = addrs[j];
			CCN_PTEH.ASID = asids[j];
			const TLB_Entry *p;
			if (find_entry(addr, &p))
				success++;
		}
	}
	double end = os_GetSeconds();
	printf("Lookup time: %f ms. Success rate %f max_len %d\n", (end - start) * 1000.0 / addrs.size(), (double)success / addrs.size() / loops, 0/*max_length*/);
}
#endif

bool UTLB_Sync(u32 entry)
{
	TLB_Entry& tlb_entry = UTLB[entry];
	u32 sz = tlb_entry.Data.SZ1 * 2 + tlb_entry.Data.SZ0;

	lru_entry = &tlb_entry;
	lru_mask = mmu_mask[sz];
	lru_address = (tlb_entry.Address.VPN << 10) & lru_mask;

	tlb_entry.Address.VPN = lru_address >> 10;
	cache_entry(tlb_entry);

	if (!mmu_enabled() && (tlb_entry.Address.VPN & (0xFC000000 >> 10)) == (0xE0000000 >> 10))
	{
		// Used when FullMMU is off
		u32 vpn_sq = ((tlb_entry.Address.VPN & 0x7FFFF) >> 10) & 0x3F;//upper bits are always known [0xE0/E1/E2/E3]
		sq_remap[vpn_sq] = tlb_entry.Data.PPN << 10;
	}
	return true;
}

void ITLB_Sync(u32 entry)
{
}

//Do a full lookup on the UTLB entry's
template<bool internal>
u32 mmu_full_lookup(u32 va, const TLB_Entry** tlb_entry_ret, u32& rv)
{
	if (lru_entry != NULL)
	{
		if (/*lru_entry->Data.V == 1 && */
				lru_address == (va & lru_mask)
				&& (lru_entry->Address.ASID == CCN_PTEH.ASID
						|| lru_entry->Data.SH == 1
						/*|| (sr.MD == 1 && CCN_MMUCR.SV == 1)*/))	// SV=1 not handled
		{
			//VPN->PPN | low bits
			// TODO mask off PPN when updating TLB to avoid doing it at look up time
			rv = ((lru_entry->Data.PPN << 10) & lru_mask) | (va & (~lru_mask));
			*tlb_entry_ret = lru_entry;

			return MMU_ERROR_NONE;
		}
	}

	if (find_entry(va, tlb_entry_ret))
	{
		u32 mask = mmu_mask[(*tlb_entry_ret)->Data.SZ1 * 2 + (*tlb_entry_ret)->Data.SZ0];
		rv = (((*tlb_entry_ret)->Data.PPN << 10) & mask) | (va & (~mask));
		lru_entry = *tlb_entry_ret;
		lru_mask = mask;
		lru_address = ((*tlb_entry_ret)->Address.VPN << 10);
		return MMU_ERROR_NONE;
	}

#ifdef USE_WINCE_HACK
	// WinCE hack
	TLB_Entry entry;
	if (wince_resolve_address(va, entry))
	{
		CCN_PTEL.reg_data = entry.Data.reg_data;
		CCN_PTEA.reg_data = entry.Assistance.reg_data;
		CCN_PTEH.reg_data = entry.Address.reg_data;
		UTLB[CCN_MMUCR.URC] = entry;

		*tlb_entry_ret = &UTLB[CCN_MMUCR.URC];
		lru_entry = *tlb_entry_ret;

		u32 sz = lru_entry->Data.SZ1 * 2 + lru_entry->Data.SZ0;
		lru_mask = mmu_mask[sz];
		lru_address = va & lru_mask;

		rv = ((lru_entry->Data.PPN << 10) & lru_mask) | (va & (~lru_mask));

		cache_entry(*lru_entry);

		return MMU_ERROR_NONE;
	}
#endif

	return MMU_ERROR_TLB_MISS;
}
template u32 mmu_full_lookup<false>(u32 va, const TLB_Entry** tlb_entry_ret, u32& rv);

template<u32 translation_type, typename T>
u32 mmu_data_translation(u32 va, u32& rv)
{
	if (va & (sizeof(T) - 1))
	{
		return MMU_ERROR_BADADDR;
	}

	if (translation_type == MMU_TT_DWRITE)
	{
		if ((va & 0xFC000000) == 0xE0000000)
		{
			u32 lookup = mmu_full_SQ<translation_type>(va, rv);
			if (lookup != MMU_ERROR_NONE)
				return lookup;

			rv = va;	//SQ writes are not translated, only write backs are.
			return MMU_ERROR_NONE;
		}
	}

//	if ((sr.MD == 0) && (va & 0x80000000) != 0)
//	{
//		//if on kernel, and not SQ addr -> error
//		return MMU_ERROR_BADADDR;
//	}

	if (sr.MD == 1 && ((va & 0xFC000000) == 0x7C000000))
	{
		rv = va;
		return MMU_ERROR_NONE;
	}

	// Not called if CCN_MMUCR.AT == 0
	//if ((CCN_MMUCR.AT == 0) || (fast_reg_lut[va >> 29] != 0))
	if (fast_reg_lut[va >> 29] != 0)
	{
		rv = va;
		return MMU_ERROR_NONE;
	}

	const TLB_Entry *entry;
	u32 lookup = mmu_full_lookup(va, &entry, rv);

//	if (lookup != MMU_ERROR_NONE)
//		return lookup;

#ifdef TRACE_WINCE_SYSCALLS
	if (unresolved_unicode_string != 0 && lookup == MMU_ERROR_NONE)
	{
		if (va == unresolved_unicode_string)
		{
			unresolved_unicode_string = 0;
			printf("RESOLVED %s\n", get_unicode_string(va).c_str());
		}
	}
#endif

//	u32 md = entry->Data.PR >> 1;
//
//	//0X  & User mode-> protection violation
//	//Priv mode protection
//	if ((md == 0) && sr.MD == 0)
//	{
//		die("MMU_ERROR_PROTECTED");
//		return MMU_ERROR_PROTECTED;
//	}
//
//	//X0 -> read olny
//	//X1 -> read/write , can be FW
//
//	//Write Protection (Lock or FW)
//	if (translation_type == MMU_TT_DWRITE)
//	{
//		if ((entry->Data.PR & 1) == 0)
//		{
//			die("MMU_ERROR_PROTECTED");
//			return MMU_ERROR_PROTECTED;
//		}
//		else if (entry->Data.D == 0)
//		{
//			die("MMU_ERROR_FIRSTWRITE");
//			return MMU_ERROR_FIRSTWRITE;
//		}
//	}

	return lookup;
}
template u32 mmu_data_translation<MMU_TT_DREAD, u8>(u32 va, u32& rv);
template u32 mmu_data_translation<MMU_TT_DREAD, u16>(u32 va, u32& rv);
template u32 mmu_data_translation<MMU_TT_DREAD, u32>(u32 va, u32& rv);
template u32 mmu_data_translation<MMU_TT_DREAD, u64>(u32 va, u32& rv);

template u32 mmu_data_translation<MMU_TT_DWRITE, u8>(u32 va, u32& rv);
template u32 mmu_data_translation<MMU_TT_DWRITE, u16>(u32 va, u32& rv);
template u32 mmu_data_translation<MMU_TT_DWRITE, u32>(u32 va, u32& rv);
template u32 mmu_data_translation<MMU_TT_DWRITE, u64>(u32 va, u32& rv);

u32 mmu_instruction_translation(u32 va, u32& rv, bool& shared)
{
	if (va & 1)
	{
		return MMU_ERROR_BADADDR;
	}
//	if ((sr.MD == 0) && (va & 0x80000000) != 0)
//	{
//		//if SQ disabled , or if if SQ on but out of SQ mem then BAD ADDR ;)
//		if (va >= 0xE0000000)
//			return MMU_ERROR_BADADDR;
//	}

	if ((CCN_MMUCR.AT == 0) || (fast_reg_lut[va >> 29] != 0))
	{
		rv = va;
		return MMU_ERROR_NONE;
	}

	// Hack fast implementation
	const TLB_Entry *tlb_entry;
	u32 lookup = mmu_full_lookup(va, &tlb_entry, rv);
	if (lookup != MMU_ERROR_NONE)
		return lookup;
	u32 md = tlb_entry->Data.PR >> 1;
	//0X  & User mode-> protection violation
	//Priv mode protection
//	if ((md == 0) && sr.MD == 0)
//	{
//		return MMU_ERROR_PROTECTED;
//	}
	shared = tlb_entry->Data.SH == 1;
	return MMU_ERROR_NONE;
}

void mmu_flush_table()
{
//	printf("MMU tables flushed\n");

//	ITLB[0].Data.V = 0;
//	ITLB[1].Data.V = 0;
//	ITLB[2].Data.V = 0;
//	ITLB[3].Data.V = 0;
//
//	for (u32 i = 0; i < 64; i++)
//		UTLB[i].Data.V = 0;

	lru_entry = NULL;
	flush_cache();
}
#endif 	// FAST_MMU
