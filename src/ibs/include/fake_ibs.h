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

#ifndef __FAKE_IBS__
#define __FAKE_IBS__

//Defined in kernel/pid.c
#define RESERVED_PIDS   300

// Comment to generate random pids
#define APP_TO_CONSIDER "streamcluster" 

//#define nr_samples_to_generate      0
#define TEST_REPLICATION            1
#define nr_samples_to_generate      20000


#define MIN_LIN_ADDR                0UL
#define MAX_LIN_ADDR                ((unsigned long) TASK_SIZE_MAX)

#define MAX_PIDS_PER_APP            64
#endif
