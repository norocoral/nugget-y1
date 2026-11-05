/***************************************************************************
 *             __________               __   ___.                  
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___  
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /  
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <   
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \  
 *                     \/            \/     \/    \/            \/ 
 *
 * Copyright (C) 2010 by Thomas Martitz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef __KERNEL_UNIX_H__
#define __KERNEL_UNIX_H__

#define HIGHEST_IRQ_LEVEL 1

int set_irq_level(int level);
void sim_enter_irq_handler(void);
void sim_exit_irq_handler(void);

#define disable_irq() \
    ((void)set_irq_level(HIGHEST_IRQ_LEVEL))

#define enable_irq() \
    ((void)set_irq_level(0))

#define disable_irq_save() \
    set_irq_level(HIGHEST_IRQ_LEVEL)

#define restore_irq(level) \
    ((void)set_irq_level(level))

void wait_for_interrupt(void);
void interrupt(void);

#endif /* __KERNEL_UNIX_H__ */
