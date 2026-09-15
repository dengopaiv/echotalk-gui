/* resource.h - control and resource identifiers for the EchoTalk GUI.
 *
 * Numbered in tab order. tools/verify_gui_keyboard.py names controls by
 * these numbers when it reports a failure, so keep the two in step. */

#ifndef ECHOTALK_GUI_RESOURCE_H
#define ECHOTALK_GUI_RESOURCE_H

#define IDC_TEXTLABEL       1000
#define IDC_TEXT            1001
#define IDC_ROMDIRLABEL     1002
#define IDC_ROMDIR          1003
#define IDC_BROWSE          1004
#define IDC_ROMSTATUSLABEL  1005
#define IDC_ROMSTATUS       1006

/* The add-on's settings, in the add-on's order. */
#define IDC_ADDONHEADING    1007
#define IDC_VOICELABEL      1008
#define IDC_VOICE           1009
#define IDC_RATELABEL       1010
#define IDC_RATE            1011
#define IDC_RATEVALUE       1012
#define IDC_PITCHLABEL      1013
#define IDC_PITCH           1014
#define IDC_PITCHVALUE      1015
#define IDC_VOLUMELABEL     1016
#define IDC_VOLUME          1017
#define IDC_VOLUMEVALUE     1018
#define IDC_DELAYLABEL      1019
#define IDC_DELAY           1020
#define IDC_DELAYVALUE      1021
#define IDC_REPEATLABEL     1022
#define IDC_REPEAT          1023
#define IDC_REPEATVALUE     1024
#define IDC_CLOCKLABEL      1025
#define IDC_CLOCK           1026
#define IDC_CLOCKVALUE      1027
#define IDC_SRATELABEL      1028
#define IDC_SRATE           1029
#define IDC_SRATEVALUE      1030
#define IDC_MONOTONE        1031
#define IDC_COMPRESSED      1032

/* What the library has and the add-on does not show. */
#define IDC_EXTRAHEADING    1033
#define IDC_FRAMERATELABEL  1034
#define IDC_FRAMERATE       1035
#define IDC_READINGLABEL    1036
#define IDC_READING         1037
#define IDC_PUNCTLABEL      1038
#define IDC_PUNCT           1039
#define IDC_CHUNKLABEL      1040
#define IDC_CHUNK           1041
#define IDC_CHUNKVALUE      1042
#define IDC_RAW             1043
#define IDC_OBEY            1044
#define IDC_PRESETLABEL     1045
#define IDC_PRESET          1046

#define IDC_PREVIEW         1047
#define IDC_STOP            1048
#define IDC_RENDER          1049
#define IDC_SAVEPRESET      1050
#define IDC_LOADPRESET      1051
#define IDC_COPYSAY         1052
#define IDC_BATCH           1053
#define IDC_SWEEP           1054
#define IDC_RESULTLABEL     1055
#define IDC_RESULT          1056

/* Private messages, posted from the render thread. */
#define WM_APP_RENDERED     (WM_APP + 1)
#define WM_APP_BATCHDONE    (WM_APP + 2)
#define WM_APP_PROGRESS     (WM_APP + 3)

#endif /* ECHOTALK_GUI_RESOURCE_H */
