/*
Copyright (C) 2013  
Fabien Gaud <fgaud@sfu.ca>, Baptiste Lepers <baptiste.lepers@inria.fr>,
Mohammad Dashti <mdashti@sfu.ca>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2, as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef _NMI_INT_H
#define _NMI_INT_H

#define IBS_OP_LOW_VALID_BIT           (1ULL<<18)      /* bit 18 */
#define IBS_OP_LOW_ENABLE              (1ULL<<17)      /* bit 17 */

struct ibs_model {
	void (*setup)(void);
	void (*shutdown)(void);
	void (*start)(void);
	void (*stop)(void);
	int (*check_ctrs)(struct pt_regs * const regs); 
};

int ibs_nmi_init(struct ibs_model *);
void ibs_nmi_exit(void);

int ibs_nmi_setup(void);
int ibs_nmi_start(void);
void ibs_nmi_stop(void);
void ibs_nmi_shutdown(void);

#endif /* _NMI_INT_H */
