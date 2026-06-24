//thank you maxem

#include "memory.h"

void *memoryAlloc( SceSize size )
{
	return memoryAllocEx( "ms_malloc", MEMORY_USER, 0, size, PSP_SMEM_Low, NULL );
}



void memoryFree( void *memblock )
{
	if( ! memblock ) return;
	 memory_header *header = (memory_header*)( (uintptr_t)memblock - sizeof(memory_header) );
	
	//return 
	sceKernelFreePartitionMemory( header->blockId );
}



void *memoryAllocEx( const char *name, MemoryPartition partition, unsigned int align, SceSize size, int type, void *addr )
{
	/* 0バイト確保しようとした場合は、最小値で確保 */
	if( ! size ) size = 1;

	memory_header header;
	void *memblock;
	/* Round size up to the next multiple of MEMORY_PAGE_SIZE.
	 * Integer math on purpose: the Allegrex has no double FPU, so ceil()/double
	 * here would drag in soft-float helpers and libm for no reason. */
	SceSize real_size = ((size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE) * MEMORY_PAGE_SIZE;
	
	header.blockId	= sceKernelAllocPartitionMemory( partition, name, type, sizeof(memory_header) + real_size + align, addr );
	header.size		= real_size; 	
	
	if( header.blockId < 0 ) return NULL;
	
	//Info分プラスしたアドレスを返す
	memblock = (void *)( (uintptr_t)(sceKernelGetBlockHeadAddr( header.blockId )) + sizeof(memory_header) );

	if( align )
	{
		if( ! MEMORY_POWER_OF_TWO( align ) )
		{
			sceKernelFreePartitionMemory( header.blockId );
			return NULL;
		}

		memblock = (void *)MEMORY_ALIGN_ADDR( align, memblock );
	}
	
	memcpy( (void *)( (uintptr_t)memblock - sizeof(memory_header) ), (void *)&header, sizeof(memory_header) );
	
	
	return memblock;
}

