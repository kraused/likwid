/*
 * =======================================================================================
 *
 *      Filename:  accessDaemon.c
 *
 *      Description:  Implementation of access daemon.
 *
 *      Version:   <VERSION>
 *      Released:  <DATE>
 *
 *      Authors:  Michael Meier, michael.meier@rrze.fau.de
 *                Jan Treibig (jt), jan.treibig@gmail.com,
 *                Thomas Roehl (tr), thomas.roehl@googlemail.com
 *      Project:  likwid
 *
 *      Copyright (C) 2016 RRZE, University Erlangen-Nuremberg
 *
 *      This program is free software: you can redistribute it and/or modify it under
 *      the terms of the GNU General Public License as published by the Free Software
 *      Foundation, either version 3 of the License, or (at your option) any later
 *      version.
 *
 *      This program is distributed in the hope that it will be useful, but WITHOUT ANY
 *      WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 *      PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License along with
 *      this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * =======================================================================================
 */

/* #####   HEADER FILE INCLUDES   ######################################### */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <syslog.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/fsuid.h>
#include <getopt.h>

#include <signal.h>
#include <sys/signalfd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <types.h>
#include <registers.h>
#include <perfmon_haswellEP_counters.h>
#include <perfmon_ivybridgeEP_counters.h>
#include <perfmon_sandybridgeEP_counters.h>
#include <perfmon_broadwelld_counters.h>
#include <perfmon_broadwellEP_counters.h>
#include <perfmon_knl_counters.h>
#include <topology.h>
#include <cpuid.h>
#include <lock.h>

/* #####   MACROS  -  LOCAL TO THIS SOURCE FILE   ######################### */

#define SA struct sockaddr
#define str(x) #x

#define CHECK_FILE_ERROR(func, msg)  \
    if ((func) == 0) { syslog(LOG_ERR, "ERROR - [%s:%d] " str(msg) " - %s \n", __FILE__, __LINE__, strerror(errno)); }

#define LOG_AND_EXIT_IF_ERROR(func, msg)  \
    if ((func) < 0) {  \
        syslog(LOG_ERR, "ERROR - [%s:%d] " str(msg) " - %s \n", __FILE__, __LINE__, strerror(errno)); \
        exit(EXIT_FAILURE); \
    }

#define PCI_ROOT_PATH    "/proc/bus/pci/"
#define MAX_PATH_LENGTH   80
//#define MAX_NUM_NODES    4

/* Lock file controlled from outside which prevents likwid to start.
 * Can be used to synchronize access to the hardware counters
 * with an external monitoring system. */

/* #####   TYPE DEFINITIONS   ########### */

typedef int (*AllowedPrototype)(uint32_t);
typedef int (*AllowedPciPrototype)(PciDeviceType, uint32_t);

/* #####   VARIABLES  -  LOCAL TO THIS SOURCE FILE   ###################### */

static int sockfd = -1;
static int connfd = -1; /* temporary in to make it compile */
static char* filepath;
static const char* ident = "accessD";
static AllowedPrototype allowed = NULL;
static AllowedPciPrototype allowedPci = NULL;
static int FD_MSR[MAX_NUM_THREADS];
static int FD_PCI[MAX_NUM_NODES][MAX_NUM_PCI_DEVICES];
static int isPCIUncore = 0;
static PciDevice* pci_devices_daemon = NULL;
static char pci_filepath[MAX_PATH_LENGTH];
static int num_pmc_counters = 0;

/* Socket to bus mapping -- will be determined at runtime;
 * typical mappings are:
 * Socket  Bus (2S)  Bus (4s)
 *   0        0xff      0x3f
 *   1        0x7f      0x7f
 *   2                  0xbf
 *   3                  0xff
 */
static char* socket_bus[MAX_NUM_NODES] = { [0 ... (MAX_NUM_NODES-1)] = NULL};

/* #####   FUNCTION DEFINITIONS  -  LOCAL TO THIS SOURCE FILE   ########### */

static int
allowed_intel(uint32_t reg)
{
    if ( ((reg & 0x0F0U) == 0x0C0U) ||
            ((reg & 0x190U) == 0x180U) ||
            ((reg & 0x190U) == 0x190U && num_pmc_counters > 4) ||
            ((reg & 0xF00U) == 0x300U) ||
            ((reg & 0xF00U) == 0xC00U) ||
            ((reg & 0xF00U) == 0xD00U) ||
            ((reg & 0xF00U) == 0xE00U) ||
            ((reg & 0xF00U) == 0xF00U) ||
            (reg == 0x1A0)  ||
            (reg == 0x1A4)  ||
            (reg == 0x0CE)  ||
            (reg == 0x19C)  ||
            (reg == 0x1A2)  ||
            (reg == 0x1AD)  ||
            (reg == 0x1AE)  ||
            (reg == 0x1AF)  ||
            (reg == 0x1AC)  ||
            (reg == 0x1A6)  ||
            (reg == 0x1A7)  ||
            (reg == 0x620)  ||
            (reg == 0xCD)   ||
            (reg == 0x1B0)  ||
            (reg == 0x1B1))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static int
allowed_sandybridge(uint32_t reg)
{
    if ((allowed_intel(reg)) ||
        (((reg & 0xF00U) == 0x600U)) ||
        (((reg & 0xF00U) == 0x700U)) ||
        (reg == MSR_ALT_PEBS))
    {
        return 1;
    }
    return 0;
}

static int
allowed_pci_sandybridge(PciDeviceType type, uint32_t reg)
{
    switch (type)
    {
        case NODEVTYPE:
            return 1;
            break;
        case R3QPI:
            if ((reg == PCI_UNC_R3QPI_PMON_BOX_CTL) ||
                (reg == PCI_UNC_R3QPI_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_R3QPI_PMON_CTL_0) ||
                (reg == PCI_UNC_R3QPI_PMON_CTL_1) ||
                (reg == PCI_UNC_R3QPI_PMON_CTL_2) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_0_A) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_1_A) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_2_A) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_0_B) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_1_B) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_2_B))
            {
                return 1;
            }
            return 0;
            break;
        case R2PCIE:
            if ((reg == PCI_UNC_R2PCIE_PMON_BOX_CTL) ||
                (reg == PCI_UNC_R2PCIE_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTL_0) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTL_1) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTL_2) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTL_3) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_0_A) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_1_A) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_2_A) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_3_A) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_0_B) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_1_B) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_2_B) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_3_B))
            {
                return 1;
            }
            return 0;
            break;
        case IMC:
            if ((reg == PCI_UNC_MC_PMON_BOX_CTL) ||
                (reg == PCI_UNC_MC_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_MC_PMON_CTL_0) ||
                (reg == PCI_UNC_MC_PMON_CTL_1) ||
                (reg == PCI_UNC_MC_PMON_CTL_2) ||
                (reg == PCI_UNC_MC_PMON_CTL_3) ||
                (reg == PCI_UNC_MC_PMON_CTR_0_A) ||
                (reg == PCI_UNC_MC_PMON_CTR_1_A) ||
                (reg == PCI_UNC_MC_PMON_CTR_2_A) ||
                (reg == PCI_UNC_MC_PMON_CTR_3_A) ||
                (reg == PCI_UNC_MC_PMON_CTR_0_B) ||
                (reg == PCI_UNC_MC_PMON_CTR_1_B) ||
                (reg == PCI_UNC_MC_PMON_CTR_2_B) ||
                (reg == PCI_UNC_MC_PMON_CTR_3_B) ||
                (reg == PCI_UNC_MC_PMON_FIXED_CTL) ||
                (reg == PCI_UNC_MC_PMON_FIXED_CTR_A) ||
                (reg == PCI_UNC_MC_PMON_FIXED_CTR_B))
            {
                return 1;
            }
            return 0;
            break;
        case HA:
            if ((reg == PCI_UNC_HA_PMON_BOX_CTL) ||
                (reg == PCI_UNC_HA_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_HA_PMON_CTL_0) ||
                (reg == PCI_UNC_HA_PMON_CTL_1) ||
                (reg == PCI_UNC_HA_PMON_CTL_2) ||
                (reg == PCI_UNC_HA_PMON_CTL_3) ||
                (reg == PCI_UNC_HA_PMON_CTR_0_A) ||
                (reg == PCI_UNC_HA_PMON_CTR_1_A) ||
                (reg == PCI_UNC_HA_PMON_CTR_2_A) ||
                (reg == PCI_UNC_HA_PMON_CTR_3_A) ||
                (reg == PCI_UNC_HA_PMON_CTR_0_B) ||
                (reg == PCI_UNC_HA_PMON_CTR_1_B) ||
                (reg == PCI_UNC_HA_PMON_CTR_2_B) ||
                (reg == PCI_UNC_HA_PMON_CTR_3_B) ||
                (reg == PCI_UNC_HA_PMON_OPCODEMATCH) ||
                (reg == PCI_UNC_HA_PMON_ADDRMATCH0) ||
                (reg == PCI_UNC_HA_PMON_ADDRMATCH1))
            {
                return 1;
            }
            return 0;
            break;
        case QPI:
            if ((reg == PCI_UNC_QPI_PMON_BOX_CTL) ||
                (reg == PCI_UNC_QPI_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_QPI_PMON_CTL_0) ||
                (reg == PCI_UNC_QPI_PMON_CTL_1) ||
                (reg == PCI_UNC_QPI_PMON_CTL_2) ||
                (reg == PCI_UNC_QPI_PMON_CTL_3) ||
                (reg == PCI_UNC_QPI_PMON_CTR_0_A) ||
                (reg == PCI_UNC_QPI_PMON_CTR_1_A) ||
                (reg == PCI_UNC_QPI_PMON_CTR_2_A) ||
                (reg == PCI_UNC_QPI_PMON_CTR_3_A) ||
                (reg == PCI_UNC_QPI_PMON_CTR_0_B) ||
                (reg == PCI_UNC_QPI_PMON_CTR_1_B) ||
                (reg == PCI_UNC_QPI_PMON_CTR_2_B) ||
                (reg == PCI_UNC_QPI_PMON_CTR_3_B) ||
                (reg == PCI_UNC_QPI_PMON_MASK_0) ||
                (reg == PCI_UNC_QPI_PMON_MASK_1) ||
                (reg == PCI_UNC_QPI_PMON_MATCH_0) ||
                (reg == PCI_UNC_QPI_PMON_MATCH_1) ||
                (reg == PCI_UNC_QPI_RATE_STATUS))
            {
                return 1;
            }
            return 0;
            break;
        case IRP:
            if ((reg == PCI_UNC_IRP_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_IRP_PMON_BOX_CTL) ||
                (reg == PCI_UNC_IRP0_PMON_CTL_0) ||
                (reg == PCI_UNC_IRP0_PMON_CTL_1) ||
                (reg == PCI_UNC_IRP0_PMON_CTR_0) ||
                (reg == PCI_UNC_IRP0_PMON_CTR_1) ||
                (reg == PCI_UNC_IRP1_PMON_CTL_0) ||
                (reg == PCI_UNC_IRP1_PMON_CTL_1) ||
                (reg == PCI_UNC_IRP1_PMON_CTR_0) ||
                (reg == PCI_UNC_IRP1_PMON_CTR_1))
            {
                return 1;
            }
            return 0;
            break;
        default:
            return 0;
            break;
    }
    return 0;
}

static int
allowed_haswell(uint32_t reg)
{
    if ((allowed_intel(reg)) ||
        (allowed_sandybridge(reg)) ||
        (((reg & 0xF00U) == 0x700U)))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static int
allowed_pci_haswell(PciDeviceType type, uint32_t reg)
{
    switch (type)
    {
        case NODEVTYPE:
            return 1;
            break;
        case R3QPI:
            if ((reg == PCI_UNC_R3QPI_PMON_BOX_CTL) ||
                (reg == PCI_UNC_R3QPI_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_R3QPI_PMON_CTL_0) ||
                (reg == PCI_UNC_R3QPI_PMON_CTL_1) ||
                (reg == PCI_UNC_R3QPI_PMON_CTL_2) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_0_A) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_1_A) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_2_A) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_0_B) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_1_B) ||
                (reg == PCI_UNC_R3QPI_PMON_CTR_2_B))
            {
                return 1;
            }
            return 0;
            break;
        case R2PCIE:
            if ((reg == PCI_UNC_R2PCIE_PMON_BOX_CTL) ||
                (reg == PCI_UNC_R2PCIE_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTL_0) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTL_1) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTL_2) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTL_3) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_0_A) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_1_A) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_2_A) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_3_A) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_0_B) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_1_B) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_2_B) ||
                (reg == PCI_UNC_R2PCIE_PMON_CTR_3_B))
            {
                return 1;
            }
            return 0;
            break;
        case IMC:
            if ((reg == PCI_UNC_MC_PMON_BOX_CTL) ||
                (reg == PCI_UNC_MC_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_MC_PMON_CTL_0) ||
                (reg == PCI_UNC_MC_PMON_CTL_1) ||
                (reg == PCI_UNC_MC_PMON_CTL_2) ||
                (reg == PCI_UNC_MC_PMON_CTL_3) ||
                (reg == PCI_UNC_MC_PMON_CTR_0_A) ||
                (reg == PCI_UNC_MC_PMON_CTR_1_A) ||
                (reg == PCI_UNC_MC_PMON_CTR_2_A) ||
                (reg == PCI_UNC_MC_PMON_CTR_3_A) ||
                (reg == PCI_UNC_MC_PMON_CTR_0_B) ||
                (reg == PCI_UNC_MC_PMON_CTR_1_B) ||
                (reg == PCI_UNC_MC_PMON_CTR_2_B) ||
                (reg == PCI_UNC_MC_PMON_CTR_3_B) ||
                (reg == PCI_UNC_MC_PMON_FIXED_CTL) ||
                (reg == PCI_UNC_MC_PMON_FIXED_CTR_A) ||
                (reg == PCI_UNC_MC_PMON_FIXED_CTR_B))
            {
                return 1;
            }
            return 0;
            break;
        case HA:
            if ((reg == PCI_UNC_HA_PMON_BOX_CTL) ||
                (reg == PCI_UNC_HA_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_HA_PMON_CTL_0) ||
                (reg == PCI_UNC_HA_PMON_CTL_1) ||
                (reg == PCI_UNC_HA_PMON_CTL_2) ||
                (reg == PCI_UNC_HA_PMON_CTL_3) ||
                (reg == PCI_UNC_HA_PMON_CTR_0_A) ||
                (reg == PCI_UNC_HA_PMON_CTR_1_A) ||
                (reg == PCI_UNC_HA_PMON_CTR_2_A) ||
                (reg == PCI_UNC_HA_PMON_CTR_3_A) ||
                (reg == PCI_UNC_HA_PMON_CTR_0_B) ||
                (reg == PCI_UNC_HA_PMON_CTR_1_B) ||
                (reg == PCI_UNC_HA_PMON_CTR_2_B) ||
                (reg == PCI_UNC_HA_PMON_CTR_3_B) ||
                (reg == PCI_UNC_HA_PMON_OPCODEMATCH) ||
                (reg == PCI_UNC_HA_PMON_ADDRMATCH0) ||
                (reg == PCI_UNC_HA_PMON_ADDRMATCH1))
            {
                return 1;
            }
            return 0;
            break;
        case QPI:
            if ((reg == PCI_UNC_V3_QPI_PMON_BOX_CTL) ||
                (reg == PCI_UNC_V3_QPI_PMON_BOX_STATUS) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTL_0) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTL_1) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTL_2) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTL_3) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTR_0_A) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTR_1_A) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTR_2_A) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTR_3_A) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTR_0_B) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTR_1_B) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTR_2_B) ||
                (reg == PCI_UNC_V3_QPI_PMON_CTR_3_B) ||
                (reg == PCI_UNC_V3_QPI_PMON_RX_MASK_0) ||
                (reg == PCI_UNC_V3_QPI_PMON_RX_MASK_1) ||
                (reg == PCI_UNC_V3_QPI_PMON_RX_MATCH_0) ||
                (reg == PCI_UNC_V3_QPI_PMON_RX_MATCH_1) ||
                (reg == PCI_UNC_V3_QPI_PMON_TX_MASK_0) ||
                (reg == PCI_UNC_V3_QPI_PMON_TX_MASK_1) ||
                (reg == PCI_UNC_V3_QPI_PMON_TX_MATCH_0) ||
                (reg == PCI_UNC_V3_QPI_PMON_TX_MATCH_1) ||
                (reg == PCI_UNC_V3_QPI_RATE_STATUS) ||
                (reg == PCI_UNC_V3_QPI_LINK_LLR) ||
                (reg == PCI_UNC_V3_QPI_LINK_IDLE))
            {
                return 1;
            }
            return 0;
            break;
        default:
            return 0;
            break;
    }
    return 0;
}

static int
allowed_silvermont(uint32_t reg)
{

    if ( ((reg & 0x0F8U) == 0x0C0U) ||
            ((reg & 0xFF0U) == 0x180U) ||
            ((reg & 0xF00U) == 0x300U) ||
            ((reg & 0xF00U) == 0x600U) ||
            ((reg & 0xF00U) == 0xC00U) ||
            ((reg & 0xF00U) == 0xD00U) ||
            (reg == 0x1A0)  ||
            (reg == 0x0CE)  ||
            (reg == 0x1AD)  ||
            (reg == 0x19C)  ||
            (reg == 0x1A2)  ||
            (reg == 0x1A6) ||
            (reg == 0x1A7))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static int allowed_knl(uint32_t reg)
{
    if (allowed_silvermont(reg))
        return 1;
    else
    {
        if (((reg & 0xF00U) == 0x700U) ||
            ((reg & 0xF00U) == 0xE00U) ||
            ((reg & 0xF00U) == 0xF00U))
            return 1;
    }
    return 0;
}

static int allowed_pci_knl(PciDeviceType type, uint32_t reg)
{
    switch(type)
    {
    case EDC:
        if ((reg == PCI_MIC2_EDC_U_CTR0_A) ||
        (reg == PCI_MIC2_EDC_U_CTR0_B) ||
            (reg == PCI_MIC2_EDC_U_CTR1_A) ||
        (reg == PCI_MIC2_EDC_U_CTR1_B) ||
            (reg == PCI_MIC2_EDC_U_CTR2_A) ||
        (reg == PCI_MIC2_EDC_U_CTR2_B) ||
            (reg == PCI_MIC2_EDC_U_CTR3_A) ||
        (reg == PCI_MIC2_EDC_U_CTR3_B) ||
        (reg == PCI_MIC2_EDC_U_CTRL0) ||
        (reg == PCI_MIC2_EDC_U_CTRL1) ||
        (reg == PCI_MIC2_EDC_U_CTRL2) ||
        (reg == PCI_MIC2_EDC_U_CTRL3) ||
        (reg == PCI_MIC2_EDC_U_BOX_CTRL) ||
        (reg == PCI_MIC2_EDC_U_BOX_STATUS) ||
        (reg == PCI_MIC2_EDC_U_FIXED_CTR_A) ||
        (reg == PCI_MIC2_EDC_U_FIXED_CTR_B) ||
        (reg == PCI_MIC2_EDC_U_FIXED_CTRL) ||
            (reg == PCI_MIC2_EDC_D_CTR0_A) ||
        (reg == PCI_MIC2_EDC_D_CTR0_B) ||
            (reg == PCI_MIC2_EDC_D_CTR1_A) ||
        (reg == PCI_MIC2_EDC_D_CTR1_B) ||
            (reg == PCI_MIC2_EDC_D_CTR2_A) ||
        (reg == PCI_MIC2_EDC_D_CTR2_B) ||
            (reg == PCI_MIC2_EDC_D_CTR3_A) ||
        (reg == PCI_MIC2_EDC_D_CTR3_B) ||
        (reg == PCI_MIC2_EDC_D_CTRL0) ||
        (reg == PCI_MIC2_EDC_D_CTRL1) ||
        (reg == PCI_MIC2_EDC_D_CTRL2) ||
        (reg == PCI_MIC2_EDC_D_CTRL3) ||
        (reg == PCI_MIC2_EDC_D_BOX_CTRL) ||
        (reg == PCI_MIC2_EDC_D_BOX_STATUS) ||
        (reg == PCI_MIC2_EDC_D_FIXED_CTR_A) ||
        (reg == PCI_MIC2_EDC_D_FIXED_CTR_B) ||
        (reg == PCI_MIC2_EDC_D_FIXED_CTRL))
        {
        return 1;
        }
        break;
    case IMC:
        if ((reg == PCI_MIC2_MC_U_CTR0_A) ||
        (reg == PCI_MIC2_MC_U_CTR0_B) ||
            (reg == PCI_MIC2_MC_U_CTR1_A) ||
        (reg == PCI_MIC2_MC_U_CTR1_B) ||
            (reg == PCI_MIC2_MC_U_CTR2_A) ||
        (reg == PCI_MIC2_MC_U_CTR2_B) ||
            (reg == PCI_MIC2_MC_U_CTR3_A) ||
        (reg == PCI_MIC2_MC_U_CTR3_B) ||
        (reg == PCI_MIC2_MC_U_CTRL0) ||
        (reg == PCI_MIC2_MC_U_CTRL1) ||
        (reg == PCI_MIC2_MC_U_CTRL2) ||
        (reg == PCI_MIC2_MC_U_CTRL3) ||
        (reg == PCI_MIC2_MC_U_BOX_CTRL) ||
        (reg == PCI_MIC2_MC_U_BOX_STATUS) ||
        (reg == PCI_MIC2_MC_U_FIXED_CTR_A) ||
        (reg == PCI_MIC2_MC_U_FIXED_CTR_B) ||
        (reg == PCI_MIC2_MC_U_FIXED_CTRL) ||
            (reg == PCI_MIC2_MC_D_CTR0_A) ||
        (reg == PCI_MIC2_MC_D_CTR0_B) ||
            (reg == PCI_MIC2_MC_D_CTR1_A) ||
        (reg == PCI_MIC2_MC_D_CTR1_B) ||
            (reg == PCI_MIC2_MC_D_CTR2_A) ||
        (reg == PCI_MIC2_MC_D_CTR2_B) ||
            (reg == PCI_MIC2_MC_D_CTR3_A) ||
        (reg == PCI_MIC2_MC_D_CTR3_B) ||
        (reg == PCI_MIC2_MC_D_CTRL0) ||
        (reg == PCI_MIC2_MC_D_CTRL1) ||
        (reg == PCI_MIC2_MC_D_CTRL2) ||
        (reg == PCI_MIC2_MC_D_CTRL3) ||
        (reg == PCI_MIC2_MC_D_BOX_CTRL) ||
        (reg == PCI_MIC2_MC_D_BOX_STATUS) ||
        (reg == PCI_MIC2_MC_D_FIXED_CTR_A) ||
        (reg == PCI_MIC2_MC_D_FIXED_CTR_B) ||
        (reg == PCI_MIC2_MC_D_FIXED_CTRL))
        {
        return 1;
        }
        break;
    case R2PCIE:
        if ((reg == PCI_MIC2_M2PCIE_CTR0_A) ||
        (reg == PCI_MIC2_M2PCIE_CTR0_B) ||
        (reg == PCI_MIC2_M2PCIE_CTR1_A) ||
                (reg == PCI_MIC2_M2PCIE_CTR1_B) ||
        (reg == PCI_MIC2_M2PCIE_CTR2_A) ||
                (reg == PCI_MIC2_M2PCIE_CTR2_B) ||
        (reg == PCI_MIC2_M2PCIE_CTR3_A) ||
                (reg == PCI_MIC2_M2PCIE_CTR3_B) ||
        (reg == PCI_MIC2_M2PCIE_CTRL0) ||
        (reg == PCI_MIC2_M2PCIE_CTRL1) ||
        (reg == PCI_MIC2_M2PCIE_CTRL2) ||
        (reg == PCI_MIC2_M2PCIE_CTRL3) ||
        (reg == PCI_MIC2_M2PCIE_BOX_CTRL) ||
        (reg == PCI_MIC2_M2PCIE_BOX_STATUS))
        {
        return 1;
        }
        break;
    case IRP:
        if ((reg == PCI_MIC2_IRP_CTR0) ||
        (reg == PCI_MIC2_IRP_CTR1) ||
        (reg == PCI_MIC2_IRP_CTRL0) ||
        (reg == PCI_MIC2_IRP_CTRL1) ||
        (reg == PCI_MIC2_IRP_BOX_CTRL) ||
        (reg == PCI_MIC2_IRP_BOX_STATUS))
        {
        return 1;
        }
        break;
    default:
        break;

    }
    return 0;
}

static int allowed_amd(uint32_t reg)
{
    if ( (reg & 0xFFFFFFF0U) == 0xC0010000U)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static int
allowed_amd15(uint32_t reg)
{
    if ( ((reg & 0xFFFFFFF0U) == 0xC0010000U) ||
            ((reg & 0xFFFFFFF0U) == 0xC0010200U) ||
            ((reg & 0xFFFFFFF8U) == 0xC0010240U))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static int
allowed_amd16(uint32_t reg)
{
    if ( ((reg & 0xFFFFFFF0U) == 0xC0010000U) ||
            ((reg & 0xFFFFFFF8U) == 0xC0010240U))
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

static void
msr_read(AccessDataRecord * dRecord)
{
    uint64_t data;
    uint32_t cpu = dRecord->cpu;
    uint32_t reg = dRecord->reg;

    dRecord->errorcode = ERR_NOERROR;
    dRecord->data = 0;

    if (!lock_check())
    {
        syslog(LOG_ERR,"Access to performance counters is locked.\n");
        dRecord->errorcode = ERR_LOCKED;
        return;
    }

    if (FD_MSR[cpu] <= 0)
    {
        dRecord->errorcode = ERR_NODEV;
        return;
    }

    if (!allowed(reg))
    {
        syslog(LOG_ERR, "Access to register 0x%X not allowed\n", reg);
        dRecord->errorcode = ERR_RESTREG;
        return;
    }

    if (pread(FD_MSR[cpu], &data, sizeof(data), reg) != sizeof(data))
    {
        syslog(LOG_ERR, "Failed to read data from register 0x%x on core %u", reg, cpu);
#ifdef DEBUG_LIKWID
        syslog(LOG_ERR, "%s", strerror(errno));
#endif
        dRecord->errorcode = ERR_RWFAIL;
        return;
    }
    dRecord->data = data;
}

static void
msr_write(AccessDataRecord * dRecord)
{
    uint32_t cpu = dRecord->cpu;
    uint32_t reg = dRecord->reg;
    uint64_t data = dRecord->data;

    dRecord->errorcode = ERR_NOERROR;

    if (!lock_check())
    {
        syslog(LOG_ERR,"Access to performance counters is locked.\n");
        dRecord->errorcode = ERR_LOCKED;
        return;
    }

    if (FD_MSR[cpu] <= 0)
    {
        dRecord->errorcode = ERR_NODEV;
        return;
    }

    if (!allowed(reg))
    {
        syslog(LOG_ERR, "Attempt to write to restricted register 0x%x on core %u", reg, cpu);
        dRecord->errorcode = ERR_RESTREG;
        return;
    }

    if (pwrite(FD_MSR[cpu], &data, sizeof(data), reg) != sizeof(data))
    {
        syslog(LOG_ERR, "Failed to write data to register 0x%x on core %u", reg, cpu);
#ifdef DEBUG_LIKWID
        syslog(LOG_ERR, "%s", strerror(errno));
#endif
        dRecord->errorcode = ERR_RWFAIL;
        return;
    }
}

static void
msr_check(AccessDataRecord * dRecord)
{
    uint32_t cpu = dRecord->cpu;
    dRecord->errorcode = ERR_NOERROR;

    if (FD_MSR[cpu] < 0)
    {
        dRecord->errorcode = ERR_NODEV;
        return;
    }
    return;
}

static void
pci_read(AccessDataRecord* dRecord)
{
    uint32_t socketId = dRecord->cpu;
    uint32_t reg = dRecord->reg;
    uint32_t device = dRecord->device;
    uint32_t data;

    dRecord->errorcode = ERR_NOERROR;
    dRecord->data = 0;

    if (!lock_check())
    {
        syslog(LOG_ERR,"Access to performance counters is locked.\n");
        dRecord->errorcode = ERR_LOCKED;
        return;
    }

    if (FD_PCI[socketId][device] == -2)
    {
        dRecord->errorcode = ERR_NODEV;
        return;
    }

    if (allowedPci)
    {
        if (!allowedPci(pci_devices_daemon[device].type, reg))
        {
        dRecord->errorcode = ERR_RESTREG;
        return;
        }
    }
    if ( !FD_PCI[socketId][device] )
    {
        snprintf(pci_filepath, MAX_PATH_LENGTH-1, "%s%s%s", PCI_ROOT_PATH, socket_bus[socketId], pci_devices_daemon[device].path);
        FD_PCI[socketId][device] = open( pci_filepath, O_RDWR);

        if ( FD_PCI[socketId][device] < 0)
        {
            syslog(LOG_ERR, "Failed to open device file %s for device %s (%s) on socket %u", pci_filepath,
                    pci_types[pci_devices_daemon[device].type].name, pci_devices_daemon[device].name, socketId);
            dRecord->errorcode = ERR_OPENFAIL;
            return;
        }
#ifdef DEBUG_LIKWID
        syslog(LOG_ERR, "Open device file %s for device %s (%s) on socket %u", pci_filepath,
                    pci_types[pci_devices_daemon[device].type].name, pci_devices_daemon[device].name, socketId);
#endif
    }

    if (FD_PCI[socketId][device] > 0 && pread(FD_PCI[socketId][device], &data, sizeof(data), reg) != sizeof(data))
    {
        syslog(LOG_ERR, "Failed to read data from pci device file %s for device %s (%s) on socket %u",
                pci_filepath,pci_types[pci_devices_daemon[device].type].name, pci_devices_daemon[device].name,socketId);
        dRecord->errorcode = ERR_RWFAIL;
        return;
    }

    dRecord->data = (uint64_t) data;
}

static void
pci_write(AccessDataRecord* dRecord)
{
    uint32_t socketId = dRecord->cpu;
    uint32_t reg = dRecord->reg;
    uint32_t device = dRecord->device;
    uint32_t data = (uint32_t) dRecord->data;

    dRecord->errorcode = ERR_NOERROR;

    if (!lock_check())
    {
        syslog(LOG_ERR,"Access to performance counters is locked.\n");
        dRecord->errorcode = ERR_LOCKED;
        return;
    }

    if (FD_PCI[socketId][device] == -2)
    {
        dRecord->errorcode = ERR_NODEV;
        return;
    }

    if (allowedPci)
    {
        if (!allowedPci(pci_devices_daemon[device].type, reg))
        {
        dRecord->errorcode = ERR_RESTREG;
        return;
        }
    }

    if ( !FD_PCI[socketId][device] )
    {
        snprintf(pci_filepath, MAX_PATH_LENGTH-1, "%s%s%s", PCI_ROOT_PATH, socket_bus[socketId], pci_devices_daemon[device].path);

        FD_PCI[socketId][device] = open( pci_filepath, O_RDWR);

        if ( FD_PCI[socketId][device] < 0)
        {
            syslog(LOG_ERR, "Failed to open device file %s for device %s (%s) on socket %u", pci_filepath,
                        pci_types[pci_devices_daemon[device].type].name, pci_devices_daemon[device].name, socketId);
            dRecord->errorcode = ERR_OPENFAIL;
            return;
        }
#ifdef DEBUG_LIKWID
        syslog(LOG_ERR, "Open device file %s for device %s (%s) on socket %u", pci_filepath,
                    pci_types[pci_devices_daemon[device].type].name, pci_devices_daemon[device].name, socketId);
#endif
    }

    if (FD_PCI[socketId][device] > 0 && pwrite(FD_PCI[socketId][device], &data, sizeof data, reg) != sizeof data)
    {
        syslog(LOG_ERR, "Failed to write data to pci device file %s for device %s (%s) on socket %u",pci_filepath,
                pci_types[pci_devices_daemon[device].type].name, pci_devices_daemon[device].name, socketId);
        dRecord->errorcode = ERR_RWFAIL;
        return;
    }
}

static void
pci_check(AccessDataRecord* dRecord)
{
    uint32_t socketId = dRecord->cpu;
    uint32_t device = dRecord->device;
    dRecord->errorcode = ERR_NOERROR;

    if (FD_PCI[socketId][device] == -2)
    {
        dRecord->errorcode = ERR_NODEV;
        return;
    }
    return;
}

static void
kill_client(void)
{
    if (connfd != -1)
    {
        CHECK_ERROR(close(connfd), socket close failed);
    }

    connfd = -1;
}

static void
stop_daemon(void)
{
    kill_client();
    for (int i=0;i<MAX_NUM_NODES;i++)
    {
        if (socket_bus[i] != NULL)
        {
            free(socket_bus[i]);
        }
    }

    if (sockfd != -1)
    {
        CHECK_ERROR(close(sockfd), socket close sockfd failed);
    }

    free(filepath);
    closelog();
    exit(EXIT_SUCCESS);
}

static int
getBusFromSocket(const uint32_t socket)
{
    int cur_bus = 0;
    uint32_t cur_socket = 0;
    char pci_filepath[1024];
    int fp;
    int ret = 0;
    while(cur_socket <= socket)
    {
        snprintf(pci_filepath, MAX_PATH_LENGTH-1, "%s%02x/05.0", PCI_ROOT_PATH, cur_bus);
        fp = open(pci_filepath, O_RDONLY);
        if (fp < 0)
        {
            return -1;
        }
        uint32_t cpubusno = 0;
        ret = pread(fp, &cpubusno, sizeof(uint32_t), 0x108);
        if (ret != sizeof(uint32_t))
        {
            close(fp);
            return -1;
        }
        cur_bus = (cpubusno >> 8) & 0x0ff;
        close(fp);
        if(socket == cur_socket)
            return cur_bus;
        ++cur_socket;
        ++cur_bus;
        if(cur_bus > 0x0ff)
           return -1;
    }

    return -1;
}

static void
Signal_Handler(int sig)
{
    if (sig == SIGPIPE)
    {
        syslog(LOG_NOTICE, "SIGPIPE? client crashed?!");
        stop_daemon();
    }

    /* For SIGALRM we just return - we're just here to create a EINTR */
    if (sig == SIGTERM)
    {
        stop_daemon();
    }
}

static void
daemonize(int* parentPid)
{
    pid_t pid, sid;

    *parentPid = getpid();

    /* already a daemon */
    if ( getppid() == 1 ) return;

    /* Fork off the parent process */
    pid = fork();

    if (pid < 0)
    {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* If we got a good PID, then we can exit the parent process. */
    if (pid > 0)
    {
        exit(EXIT_SUCCESS);
    }

    /* At this point we are executing as the child process */

    /* Create a new SID for the child process */
    sid = setsid();

    if (sid < 0)
    {
        syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Change the current working directory.  This prevents the current
       directory from being locked; hence not being able to remove it. */
    if ((chdir("/")) < 0)
    {
        syslog(LOG_ERR, "chdir failed:  %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Redirect standard files to /dev/null */
    {
        CHECK_FILE_ERROR(freopen( "/dev/null", "r", stdin), freopen stdin failed);
        CHECK_FILE_ERROR(freopen( "/dev/null", "w", stdout), freopen stdout failed);
        CHECK_FILE_ERROR(freopen( "/dev/null", "w", stderr), freopen stderr failed);
    }
}

int serviceClient(int connfd)
{
    int ret;
    uint32_t numHWThreads = sysconf(_SC_NPROCESSORS_CONF);
    uint32_t model;
    AccessDataRecord dRecord;

    for (int i=0;i<MAX_NUM_THREADS;i++)
    {
        FD_MSR[i] = -1;
    }

    {
        uint32_t  eax = 0x00;
        uint32_t  ebx = 0x00;
        uint32_t  ecx = 0x00;
        uint32_t  edx = 0x00;
        /*int isIntel = 1;
        CPUID(eax, ebx, ecx, edx);
        if (ebx == 0x68747541U)
        {
            isIntel = 0;
        }*/

        eax = 0x01;
        CPUID(eax, ebx, ecx, edx);
        uint32_t family = ((eax >> 8) & 0xFU) + ((eax >> 20) & 0xFFU);
        model  = (((eax >> 16) & 0xFU) << 4) + ((eax >> 4) & 0xFU);
        eax = 0x0A;
        CPUID(eax, ebx, ecx, edx);
        num_pmc_counters = (int)((eax>>8)&0xFFU);

        switch (family)
        {
            case P6_FAMILY:
                allowed = allowed_intel;

                if ((model == SANDYBRIDGE) || (model == IVYBRIDGE))
                {
                    allowed = allowed_sandybridge;
                }
                else if ((model == SANDYBRIDGE_EP) || (model == IVYBRIDGE_EP))
                {
                    allowed = allowed_sandybridge;
                    allowedPci = allowed_pci_sandybridge;
                    isPCIUncore = 1;
                }
                else if ((model == HASWELL) ||
                         (model == HASWELL_M1) ||
                         (model == HASWELL_M2) ||
                         (model == BROADWELL) ||
                         (model == SKYLAKE1) ||
                         (model == SKYLAKE2) ||
                         (model == KABYLAKE1) ||
                         (model == KABYLAKE2))
                {
                    allowed = allowed_sandybridge;
                }
                else if (model == BROADWELL_D)
                {
                    allowed = allowed_sandybridge;
                    isPCIUncore = 1;
                    allowedPci = allowed_pci_haswell;
                }
                else if (model == HASWELL_EP)
                {
                    isPCIUncore = 1;
                    allowed = allowed_sandybridge;
                    allowedPci = allowed_pci_haswell;
                }
                else if (model == BROADWELL_E)
                {
                    isPCIUncore = 1;
                    allowed = allowed_sandybridge;
                    allowedPci = allowed_pci_haswell;
                }
                else if ((model == ATOM_SILVERMONT_C) ||
                         (model == ATOM_SILVERMONT_E) ||
                         (model == ATOM_SILVERMONT_Z1) ||
                         (model == ATOM_SILVERMONT_Z2) ||
                         (model == ATOM_SILVERMONT_F) ||
                         (model == ATOM_SILVERMONT_AIR))
                {
                    allowed = allowed_silvermont;
                }
                else if (model == XEON_PHI_KNL)
                {
                    allowed = allowed_knl;
                    isPCIUncore = 1;
                    allowedPci = allowed_pci_knl;
                }
                break;
            case K8_FAMILY:
            case K10_FAMILY:
                allowed = allowed_amd;
                break;
            case K15_FAMILY:
                allowed = allowed_amd15;
                break;
            case K16_FAMILY:
                allowed = allowed_amd16;
            break;
            default:
                syslog(LOG_ERR, "ERROR - [%s:%d] - Unsupported processor. Exiting!  \n",
                        __FILE__, __LINE__);
                exit(EXIT_FAILURE);
        }
    }

    {
        char* msr_file_name = (char*) malloc(MAX_PATH_LENGTH * sizeof(char));

        /* Open MSR device files for less overhead.
         * NOTICE: This assumes consecutive processor Ids! */
        for ( uint32_t i=0; i < numHWThreads; i++ )
        {
            sprintf(msr_file_name,"/dev/cpu/%d/msr",i);
            FD_MSR[i] = open(msr_file_name, O_RDWR);

            if ( FD_MSR[i] < 0 )
            {
                syslog(LOG_ERR, "Failed to open device file %s: %s, trying /dev/msr%d", msr_file_name, strerror(errno), i);
                sprintf(msr_file_name,"/dev/msr%d",i);
                FD_MSR[i] = open(msr_file_name, O_RDWR);
                if ( FD_MSR[i] < 0 )
                {
                    syslog(LOG_ERR, "Failed to open device file %s: %s.", msr_file_name, strerror(errno));
                }
            }
        }

        free(msr_file_name);
        if (isPCIUncore)
        {
            int cntr = 0;
            int socket_count = 0;
            if (model == SANDYBRIDGE_EP)
            {
                //testDevice = 0x80863c44;
                pci_devices_daemon = sandybridgeEP_pci_devices;
            }
            else if (model == IVYBRIDGE_EP)
            {
                //testDevice = 0x80860e36;
                pci_devices_daemon = ivybridgeEP_pci_devices;
            }
            else if (model == HASWELL_EP)
            {
                //testDevice = 0x80862f30;
                pci_devices_daemon = haswellEP_pci_devices;
            }
            else if (model == BROADWELL_D)
            {
                //testDevice = 0x80862f30;
                pci_devices_daemon = broadwelld_pci_devices;
            }
            else if (model == BROADWELL_E)
            {
                //testDevice = 0x80862f30;
                pci_devices_daemon = broadwellEP_pci_devices;
            }
            else if (model == XEON_PHI_KNL)
            {
                pci_devices_daemon = knl_pci_devices;
            }
            else
            {
                //testDevice = 0;
                syslog(LOG_NOTICE, "PCI Uncore not supported on this system");
                goto LOOP;
            }

            for (int j=0; j<MAX_NUM_NODES; j++)
            {
                socket_bus[j] = (char*)malloc(4);
                sprintf(socket_bus[j], "N-A");
                for (int i=0; i<MAX_NUM_PCI_DEVICES; i++)
                {
                    FD_PCI[j][i] = -2;
                }
            }

            /* determine PCI-BUSID mapping ... */
            int sbus = -1;
            cntr = 0;
            sbus = getBusFromSocket(cntr);
            while (sbus != -1)
            {
                sprintf(socket_bus[cntr], "%02x/", sbus);
                cntr++;
                sbus = getBusFromSocket(cntr);
            }

            if ( cntr == 0 )
            {
                syslog(LOG_NOTICE, "Uncore not supported on this system");
            }
            else
            {
                socket_count = cntr;
                int fd;
                for (int j=0; j<socket_count; j++)
                {
                    for (int i=1; i<MAX_NUM_PCI_DEVICES; i++)
                    {
                        if (pci_devices_daemon[i].path)
                        {
                            sprintf(pci_filepath, "%s%s%s", PCI_ROOT_PATH, socket_bus[j], pci_devices_daemon[i].path);
                            fd = open(pci_filepath, O_RDWR);
                            if (fd > 0)
                            {
                                FD_PCI[j][i] = 0;
                                pci_devices_daemon[i].online = 1;
                                close(fd);
                            }
#ifdef DEBUG_LIKWID
                            else if (j==0)
                            {
                                syslog(LOG_NOTICE, "Device %s for socket %d not found at path %s, excluded it from device list: %s\n",pci_devices_daemon[i].name,j, pci_filepath, strerror(errno));
                            }
#endif
                        }
                    }
                }
            }
        }
    }
LOOP:
    while (1)
    {
        ret = read(connfd, (void*) &dRecord, sizeof(AccessDataRecord));

        if (ret < 0)
        {
            stop_daemon();
        }
        else if ((ret == 0) && (dRecord.type != DAEMON_EXIT))
        {
            syslog(LOG_ERR, "ERROR - [%s:%d] zero read", __FILE__, __LINE__);
            stop_daemon();
        }
        else if (ret != sizeof(AccessDataRecord))
        {
            syslog(LOG_ERR, "ERROR - [%s:%d] unaligned read", __FILE__, __LINE__);
            stop_daemon();
        }


        if (dRecord.type == DAEMON_READ)
        {
            if (dRecord.device == MSR_DEV)
            {
                msr_read(&dRecord);
            }
            else
            {
                pci_read(&dRecord);
            }
        }
        else if (dRecord.type == DAEMON_WRITE)
        {
            if (dRecord.device == MSR_DEV)
            {
                msr_write(&dRecord);
                dRecord.data = 0x0ULL;
            }
            else
            {
                pci_write(&dRecord);
                dRecord.data = 0x0ULL;
            }
        }
        else if (dRecord.type == DAEMON_CHECK)
        {
            if (dRecord.device == MSR_DEV)
            {
                msr_check(&dRecord);
            }
            else
            {
                pci_check(&dRecord);
            }
        }
        else if (dRecord.type == DAEMON_EXIT)
        {
            stop_daemon();
        }
        else
        {
            syslog(LOG_ERR, "unknown daemon access type  %d", dRecord.type);
            dRecord.errorcode = ERR_UNKNOWN;
        }

        LOG_AND_EXIT_IF_ERROR(write(connfd, (void*) &dRecord, sizeof(AccessDataRecord)), write failed);
    }

    /* never reached */
    return EXIT_SUCCESS;
}

/* #####  MAIN FUNCTION DEFINITION   ################## */

int main(void)
{
    pid_t pid;
#if 0
    mode_t oldumask;
#endif
    struct sockaddr_un  addr1;
    socklen_t socklen;
    sigset_t all;
    int err;
    int sigfd;

    openlog(ident, 0, LOG_USER);

    if (!lock_check())
    {
        syslog(LOG_ERR,"Access to performance counters is locked.\n");
        stop_daemon();
    }

#if 0
    daemonize(&pid);
    syslog(LOG_INFO, "AccessDaemon runs with UID %d, eUID %d\n", getuid(), geteuid());
#endif

    pid = getpid();
    syslog(LOG_INFO, "AccessDaemon is running with pid %d", pid);

    sigfillset(&all);
    LOG_AND_EXIT_IF_ERROR(sigprocmask(SIG_BLOCK, &all, NULL), sigprocmask failed);

    LOG_AND_EXIT_IF_ERROR(sigfd = signalfd(-1, &all, 0), signalfd failed);

    /* setup filename for socket */
    filepath = (char*) calloc(sizeof(addr1.sun_path), 1);
    snprintf(filepath, sizeof(addr1.sun_path), TOSTRING(LIKWIDSOCKETBASE) "-d");

    /* get a socket */
    LOG_AND_EXIT_IF_ERROR(sockfd = socket(AF_LOCAL, SOCK_STREAM, 0), socket failed);

    /* initialize socket data structure */
    bzero(&addr1, sizeof(addr1));
    addr1.sun_family = AF_LOCAL;
    strncpy(addr1.sun_path, filepath, (sizeof(addr1.sun_path) - 1)); /* null terminated by the bzero() above! */

    /* bind and listen on socket */
    LOG_AND_EXIT_IF_ERROR(bind(sockfd, (SA*) &addr1, sizeof(addr1)), bind failed);
    LOG_AND_EXIT_IF_ERROR(listen(sockfd, 1), listen failed);
    LOG_AND_EXIT_IF_ERROR(chmod(filepath, S_IROTH|S_IWOTH), chmod failed);

    while (1)
    {
        struct pollfd pfs[2];
        pfs[0].fd     = sigfd;
        pfs[0].events = POLLIN;
        pfs[1].fd     = sockfd;
        pfs[1].events = POLLIN;

        LOG_AND_EXIT_IF_ERROR(poll(pfs, sizeof(pfs)/sizeof(pfs[0]), -1), poll failed);

        if (pfs[0].revents & POLLIN) {
            struct signalfd_siginfo info;

            err = read(sigfd, &info, sizeof(info));
            LOG_AND_EXIT_IF_ERROR(err, read failed);
            LOG_AND_EXIT_IF_ERROR(((int )err - (int )sizeof(info)), short read);

            if ((SIGCHLD == info.ssi_signo)) {
                syslog(LOG_INFO, "Child with pid %d is gone\n", info.ssi_pid);
                waitpid(info.ssi_pid, NULL, 0);
            }
            if ((SIGINT  == info.ssi_signo) ||
                (SIGQUIT == info.ssi_signo) ||
                (SIGTERM == info.ssi_signo)) {
                /* TODO One might want to cleanup child processes here. In the scenario that I will
                 *      deploy this code it will be done externally anyway.
                 */
                CHECK_ERROR(unlink(filepath), unlink of socket failed);
                syslog(LOG_INFO, "AccessDaemon terminates");
                return 0;
            }
        }

        if (pfs[1].revents & POLLIN) {
            socklen = sizeof(addr1);
            LOG_AND_EXIT_IF_ERROR(connfd = accept(sockfd, (SA*) &addr1, &socklen), accept failed);

           pid = fork();
           if (0 == pid) {
               serviceClient(connfd);
               /* never reached */
               return EXIT_SUCCESS;
           }
           syslog(LOG_INFO, "Child with pid %d is alive\n", pid);
        }
   }

    /* never reached */
    return EXIT_SUCCESS;
}

