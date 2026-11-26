#pragma once
// Resource IDs for the Launcher picker dialog (Launcher.dll).
#define IDD_PICKER     101
#define IDC_LIST       1001
#define IDC_EDIT       1002
#define IDC_LAUNCH     1003
#define IDC_FILTER     1004   // "Filter:" edit - shows only missions starting with its text
#define IDC_LISTLABEL  1005   // the list's caption, updated with the match count
// Mission details: up to INFO_ROWS single-line labels, IDC_INFO0 + row, created at run time by
// picker.cpp's SetInfo (not in picker.rc) - a fresh set for each mission.
#define IDC_INFO0      1100
#define INFO_ROWS      18
// Cancel uses the standard IDCANCEL (2).
