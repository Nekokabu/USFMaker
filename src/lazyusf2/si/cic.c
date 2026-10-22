/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - cic.c                                                   *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
 *   Copyright (C) 2014 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "usf/usf.h"

#include "cic.h"

#include "api/m64p_types.h"
#include "api/callbacks.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>


void init_cic_using_ipl3(usf_state_t * state, struct cic* cic, const void* ipl3)
{
    size_t i;
    unsigned long long crc = 0;

    static const struct cic cics[] =
    {
        { CIC_X101, 0x3f },
        { CIC_X102, 0x3f },
        { CIC_X103, 0x78 },
        { CIC_X105, 0x91 },
        { CIC_X106, 0x85 },
        { CIC_5101, 0xac }   /* USFX: Aleck64 */
    };
    unsigned long long crc_c00 = 0;

    for (i = 0; i < 0xfc0/4; i++)
    {
        if (i == (0xc00 - 0x40) / 4)
            crc_c00 = crc;
        crc += ((uint32_t*)ipl3)[i];
    }

    /* USFX: the Aleck64 IPL3 ends at 0xC00; what follows differs between
       games (Project64 sums 0x40-0xBFF, mupen64plus 0x40-0xFFF) */
    if (crc == 0x000000A5F80BF620LL || crc_c00 == 0x000000A5F80BF620LL)
    {
        memcpy(cic, &cics[5], sizeof(*cic));
        return;
    }

    switch(crc)
    {
        default:
            DebugMessage(state, M64MSG_WARNING, "Unknown CIC type (%08x)! using CIC 6102.", crc);
        case 0x000000D057C85244LL: i = 1; break; /* CIC_X102 */
        case 0x000000D0027FDF31LL:               /* CIC_X101 */
        case 0x000000CFFB631223LL: i = 0; break; /* CIC_X101 */
        case 0x000000D6497E414BLL: i = 2; break; /* CIC_X103 */
        case 0x0000011A49F60E96LL: i = 3; break; /* CIC_X105 */
        case 0x000000D6D5BE5580LL: i = 4; break; /* CIC_X106 */
    }

    memcpy(cic, &cics[i], sizeof(*cic));
}

