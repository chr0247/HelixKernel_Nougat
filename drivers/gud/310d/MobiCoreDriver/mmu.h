/*
 * Copyright (c) 2013-2015 TRUSTONIC LIMITED
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef _TBASE_MEM_H_
#define _TBASE_MEM_H_

struct tee_mmu;
struct mcp_buffer_map;

struct tee_mmu *tee_mmu_create(struct task_struct *task, const void *wsm_buffer,
			       unsigned int wsm_len);

void tee_mmu_delete(struct tee_mmu *mmu);

void tee_mmu_buffer(const struct tee_mmu *mmu, struct mcp_buffer_map *map);

int tee_mmu_debug_structs(struct kasnprintf_buf *buf,
			  const struct tee_mmu *mmu);

#endif 
