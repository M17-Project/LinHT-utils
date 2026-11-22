// linht-gnssd tests: recorded NMEA

#ifndef GNSSD_FIXTURES_H
#define GNSSD_FIXTURES_H

// LinHT Rev C, indoors, 2026-11-20, one 1 s epoch without a fix. The
// receiver keeps the date from an earlier fix in its RTC.
static const char *const fx_nofix[] = {
    "$GNRMC,065403.132,V,,,,,0.00,0.00,201126,,,N,V*2B",
    "$GNVTG,0.00,T,,M,0.00,N,0.00,K,N*2C",
    "$GNGGA,065403.132,,,,,0,0,,,M,,M,,*52",
    "$GNGSA,A,1,,,,,,,,,,,,,,,,1*1D",
    "$GNGSA,A,1,,,,,,,,,,,,,,,,2*1E",
    "$GPGSV,1,1,00,1*64",
    "$GLGSV,1,1,00,1*78",
    "$GNGLL,,,,,065403.132,V,N*60",
    "$GPTXT,01,01,02,ANTSTATUS=OPEN*2B",
    NULL,
};

// LinHT Rev B the same day: no time in the RTC yet (GPS epoch 1980-01-06).
static const char *const fx_nofix_notime[] = {
    "$GNRMC,172129.870,V,,,,,0.00,0.00,060180,,,N,V*27",
    "$GNGGA,172129.870,,,,,0,0,,,M,,M,,*57",
    NULL,
};

// 3D fix, GPS + GLONASS, from the LG77L (C) examples of the Quectel
// protocol specification V2.0 (RMC, GGA, GSA, GSV) in one epoch.
static const char *const fx_fix3d[] = {
    "$GNRMC,080237.000,A,3149.333190,N,11706.911552,E,0.00,5.78,221121,,,D,V*06",
    "$GNGGA,080237.000,3149.333190,N,11706.911552,E,2,15,0.74,53.489,M,-0.337,M,,*5F",
    "$GNGSA,A,3,22,29,26,25,03,32,31,16,194,193,,,1.35,0.74,1.13,1*0E",
    "$GNGSA,A,3,66,65,76,67,77,,,,,,,,1.35,0.74,1.13,2*06",
    "$GPGSV,4,1,13,194,72,074,43,26,61,222,45,31,61,352,43,32,60,116,47,1*5A",
    "$GPGSV,4,2,13,50,51,161,39,193,49,150,41,22,30,288,41,29,26,082,41,1*5E",
    "$GPGSV,4,3,13,25,23,042,38,03,20,311,38,16,19,215,38,10,09,175,,1*6A",
    "$GPGSV,4,4,13,195,,,,1*5B",
    "$GLGSV,2,1,06,66,69,316,46,76,62,331,44,65,43,162,37,77,34,255,33,1*70",
    "$GLGSV,2,2,06,67,16,330,40,85,04,074,32,1*77",
    "$GPTXT,01,01,02,ANTSTATUS=OK*3B",
    NULL,
};

// Replies of the Rev C receiver (firmware LG77LICNR11A01S_GLN).
static const char *const fx_replies[] = {
    "$PQVERNO,R,LG77LICNR11A01S_GLN,2023/01/16,01:33*75",
    "$PMTK514,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*32",
    "$PMTK869,2,1,0*2A",
    NULL,
};

#endif
