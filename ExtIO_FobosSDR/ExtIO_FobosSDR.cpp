 
#define EXTIO_EXPORTS		1
#define HWNAME				"Fobos SDR"
#define HWMODEL				"Fobos SDR"
#define SETTINGS_IDENTIFIER	"Fobos SDR"
#define MAX_DEVICES         64
#define LO_MIN				50000000LL
#define LO_MAX				6000000000LL
#define EXT_BLOCKLEN		(4096*8)

#include "ExtIO_FobosSDR.h"
#include "fobos.h"
#include "resource.h"

//==============================================================================
#include <windows.h>
#include <windowsx.h>
#include <string.h>
#include <string>
#include <commctrl.h>
#include <stdio.h>
#include <process.h>
#include <conio.h>
//==============================================================================

#pragma warning(disable : 4996)
#pragma comment(lib, "fobos.lib")
#define snprintf	_snprintf

static HMODULE hInst;

typedef struct sr 
{
    double value;
    const wchar_t* name;
} sr_t;

static sr_t gSampleRates[] = 
{
	{  8000000.0, L"8 MHz" },
    { 10000000.0, L"10 MHz" },
	{ 12500000.0, L"12.5 MHz" },
    { 16000000.0, L"16 MHz" },
    { 20000000.0, L"20 MHz" },
	{ 25000000.0, L"25 MHz" },
    { 32000000.0, L"32 MHz" },
    { 40000000.0, L"40 MHz" },
    { 50000000.0, L"50 MHz" }
};

static bool SDR_supports_settings = false;  // assume not supported
static bool SDR_settings_valid = false;		// assume settings are for some other ExtIO

static char SDR_progname[32+1] = "\0";
static int  SDR_ver_major = -1;
static int  SDR_ver_minor = -1;

fobos_dev_t* gDev = NULL;
static int giDeviceCount = 0;
static int giDeviceIdx = 0;
static char gSerials[MAX_DEVICES][64];
static int giStreaming;
static char gBoardInfo[64];

static HANDLE ghWorker = INVALID_HANDLE_VALUE;

#define USER_GPO0		1
#define USER_GPO1		2
#define USER_GPO2		4
#define USER_GPO3		8
#define USER_GPO4		8
#define USER_GPO5		8
#define USER_GPO6		8
#define USER_GPO7		8

static int64_t 	gdLOfreq = 100000000;
static int		giSrateIdx = 9; // 
static int		giSamplingMode = 0;
static int		giExternalClock = 0;
static int		giLnaGain = 0;
static int		giVgaGain = 0;
static uint8_t	giUserGPO = 0;

static bool		gbInitHW = false;
static bool		gbStartHW = false;
static bool		gbChangeHW = false;

extern pfnExtIOCallback	pfnCallback = NULL;

HWND ghDialog = nullptr;
//==============================================================================
void RxCallBack(float* buf, uint32_t len, void* ctx)
{
    giStreaming = 1;
    if (pfnCallback)
    {
        if (giSamplingMode == 2)
        {
            int count = len / 4;
            for (int i = 0; i < count; i++)
            {
                // re
                buf[i * 8 + 1] = 0.0f;             // im = 0

                buf[i * 8 + 2] = -buf[i * 8 + 2];  // re = -re
                buf[i * 8 + 3] = 0.0f;             // im = 0

                                                   // re
                buf[i * 8 + 5] = 0.0f;             // im = 0

                buf[i * 8 + 6] = -buf[i * 8 + 6];  // re = -re
                buf[i * 8 + 7] = 0.0f;             // im = 0
            }
        }
        if (giSamplingMode == 3)
        {
            int count = len / 4;
            for (int i = 0; i < count; i++)
            {
                buf[i * 8 + 0] = buf[i * 8 + 1];   // re = im
                buf[i * 8 + 1] = 0.0f;             // im = 0

                buf[i * 8 + 2] = - buf[i * 8 + 3]; // re = -im
                buf[i * 8 + 3] = 0.0f;             // im = 0

                buf[i * 8 + 4] = buf[i * 8 + 5];   // re = im
                buf[i * 8 + 5] = 0.0f;             // im = 0

                buf[i * 8 + 6] = - buf[i * 8 + 7]; // re = -im
                buf[i * 8 + 7] = 0.0f;             // im = 0
            }
        }
        pfnCallback(EXT_BLOCKLEN, 0, 0.0F, buf);
    }
}
//==============================================================================
void UpdateDialog()
{
    if (ghDialog == nullptr) return;
    ComboBox_SetCurSel(GetDlgItem(ghDialog, IDC_COMBO_DEVICE), giDeviceIdx);// Device dropdown
    EnableWindow(GetDlgItem(ghDialog, IDC_COMBO_DEVICE), !giStreaming);
    ComboBox_SetCurSel(GetDlgItem(ghDialog, IDC_COMBO_SR), giSrateIdx);  // Samplerate dropdown
    EnableWindow(GetDlgItem(ghDialog, IDC_COMBO_SR), !giStreaming);

    SetWindowTextA(GetDlgItem(ghDialog, IDC_EDIT_BOARD), gBoardInfo);
    SetWindowTextA(GetDlgItem(ghDialog, IDC_EDIT_SERIAL), gSerials[giDeviceIdx]);

    ComboBox_SetCurSel(GetDlgItem(ghDialog, IDC_COMBO_SAMPLING_MODE), giSamplingMode);

    Button_SetCheck(GetDlgItem(ghDialog, IDC_CHECK_EXT_CLOCK), giExternalClock);

    /* GPO checkbox */
    Button_SetCheck(GetDlgItem(ghDialog, IDC_CHECK_GPO0), ((giUserGPO & 0x01) == 0x01));
    Button_SetCheck(GetDlgItem(ghDialog, IDC_CHECK_GPO1), ((giUserGPO & 0x02) == 0x02));
    Button_SetCheck(GetDlgItem(ghDialog, IDC_CHECK_GPO1), ((giUserGPO & 0x04) == 0x04));
    Button_SetCheck(GetDlgItem(ghDialog, IDC_CHECK_GPO1), ((giUserGPO & 0x08) == 0x08));
    Button_SetCheck(GetDlgItem(ghDialog, IDC_CHECK_GPO0), ((giUserGPO & 0x10) == 0x10));
    Button_SetCheck(GetDlgItem(ghDialog, IDC_CHECK_GPO1), ((giUserGPO & 0x20) == 0x20));
    Button_SetCheck(GetDlgItem(ghDialog, IDC_CHECK_GPO1), ((giUserGPO & 0x40) == 0x40));
    Button_SetCheck(GetDlgItem(ghDialog, IDC_CHECK_GPO1), ((giUserGPO & 0x80) == 0x80));

    /* Update LNA slider */
    SendDlgItemMessage(ghDialog, IDC_SLIDER_GAIN_LNA, TBM_SETPOS, TRUE, ((int)giLnaGain));

    /* Update PGA slider */
    SendDlgItemMessage(ghDialog, IDC_SLIDER_GAIN_VGA, TBM_SETPOS, TRUE, ((int)giVgaGain));
}
//==============================================================================
unsigned int __stdcall ThreadProc(void* p)
{
    int r = fobos_rx_read_async(gDev, RxCallBack, NULL, 16, EXT_BLOCKLEN);
    return r;
}
//==============================================================================
static int StartThread()
{
    //If already running, exit
	if (ghWorker != INVALID_HANDLE_VALUE)
	{
		return -1;
	}
    giStreaming = 1;
    UpdateDialog();

    ghWorker = (HANDLE)_beginthreadex(0, 0, &ThreadProc, 0, 0, 0);

	if (ghWorker == INVALID_HANDLE_VALUE)
	{
        return -1;
	}

    SetThreadPriority(ghWorker, THREAD_PRIORITY_TIME_CRITICAL);
    return 0;
}
//==============================================================================
static int StopThread()
{
	if (ghWorker == INVALID_HANDLE_VALUE)
	{
		return -1;
	}
    
    int r = fobos_rx_cancel_async(gDev);
    Sleep(500);
    WaitForSingleObject(ghWorker, INFINITE);
    CloseHandle(ghWorker);
    ghWorker = INVALID_HANDLE_VALUE;
    giStreaming = 0;
    UpdateDialog();
    return r;
}
//==============================================================================
static INT_PTR CALLBACK MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    //cprintf("m");
    switch (uMsg)
    {
        /* Init starting variables */
        case WM_INITDIALOG:
        {
            char info [128];
            /* Add device choices */
            for (int i = 0; i < giDeviceCount; i++)
            {
                sprintf(info, "Fobos SDR %s", gSerials[i]);
                wchar_t winfo[128];
                MultiByteToWideChar(CP_UTF8, 0, info, -1, winfo, 128);
                ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_DEVICE), winfo);
            }

            char lib_version[32];
            char drv_version[32];
            fobos_rx_get_api_info(lib_version, drv_version);
            sprintf(info, "lib v.%s drv %s", lib_version, drv_version);
            SetWindowTextA(GetDlgItem(hwndDlg, IDC_EDIT_API), info);


            /* Add samplerate choices */
            for (int i = 0; i < (sizeof(gSampleRates) / sizeof(gSampleRates[0])); i++)
            {
                ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_SR), gSampleRates[i].name);
            }

            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_SAMPLING_MODE), L"RF");
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_SAMPLING_MODE), L"IQ (HF1+HF2) direct sampling");
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_SAMPLING_MODE), L"HF1 direct sampling");
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_SAMPLING_MODE), L"HF2 direct sampling");


            /* Add tickmarks, set range for LNA slider */
            SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_LNA, TBM_SETRANGEMIN, FALSE, 0);
            SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_LNA, TBM_SETRANGEMAX, FALSE, 3);
            for (int i = 0; i <= 3; i++)
            {
                SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_LNA, TBM_SETTIC, FALSE, i);
            }

            /* Add tickmarks, set range for VGA slider */
            SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_VGA, TBM_SETRANGEMIN, FALSE, 0);
            SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_VGA, TBM_SETRANGEMAX, FALSE, 15);
            for (int i = 0; i <= 15; i++)
            {
                SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_VGA, TBM_SETTIC, FALSE, i);
            }

            UpdateDialog();
            return TRUE;
        }
        break;

        /* Update dialog */
        case WM_SHOWWINDOW:
        {
            UpdateDialog();
            return TRUE;
        }
        break;
        /* Scroll message */
        case WM_HSCROLL:
        {
            /* LNA slider moved */
            if (GetDlgItem(hwndDlg, IDC_SLIDER_GAIN_LNA) == (HWND)lParam)
            {
                if (giLnaGain != SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_LNA, TBM_GETPOS, 0, NULL))
                {
                    giLnaGain = SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_LNA, TBM_GETPOS, 0, NULL);

                    UpdateDialog();

                    if (giLnaGain <= 15)
                    {
                        int r = fobos_rx_set_lna_gain(gDev, giLnaGain);
                        if (r != 0)
                            return FALSE;
                    }

                    pfnCallback(-1, extHw_Changed_ATT, 0, NULL);
                    return TRUE;
                }
            }
            /* VGA slider moved */
            if (GetDlgItem(hwndDlg, IDC_SLIDER_GAIN_PGA) == (HWND)lParam)
            {
                if (giVgaGain != SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_PGA, TBM_GETPOS, 0, NULL))
                {
                    giVgaGain = SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_PGA, TBM_GETPOS, 0, NULL);

                    UpdateDialog();

                    if (giVgaGain <= 15)
                    {
                        int r = fobos_rx_set_vga_gain(gDev, giVgaGain);
                        if (r != 0)
                            return FALSE;
                    }

                    pfnCallback(-1, extHw_Changed_ATT, 0, NULL);
                    return TRUE;
                }
            }
        }
        break;
        /* Command message */
        case WM_COMMAND:
        {
            DWORD CmdId = GET_WM_COMMAND_ID(wParam, lParam);
            switch (CmdId)
			{
                // Changed device
                case IDC_COMBO_DEVICE:
                {
                    if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
                    {
                        if (giDeviceIdx != ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam)))
                        {

                            gbChangeHW = true;
                            int64_t freq = -1;
                            if (gbStartHW)
                            {
                                freq = GetHWLO64();
                                StopHW();
                                CloseHW();
                            }
                            giDeviceIdx = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));
                            if (OpenHW() && freq != -1)
                            {
                                StartHW64(freq);
                                if (!gbStartHW)
                                    MessageBox(NULL, TEXT("Device change error"), TEXT("ExtIO"), MB_ICONERROR | MB_OK);
                            }
                            gbChangeHW = false;

                            UpdateDialog();

                            pfnCallback(-1, extHw_Changed_SampleRate, 0, NULL);
                            pfnCallback(-1, extHw_Changed_ATT, 0, NULL);

                            return TRUE;
                        }
                    }
                }
                break;
                // Changed samle rate
                case IDC_COMBO_SR:
                {
                    if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
                    {
                        int sr = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));
                        if (giSrateIdx != sr) 
                        {
                            if (sr >= 0 && sr < (sizeof(gSampleRates) / sizeof(gSampleRates[0])))
                            {
                                giSrateIdx = sr;
                                int r = fobos_rx_set_samplerate(gDev, gSampleRates[giSrateIdx].value, 0);
                                if (r != 0)
                                    return FALSE;

                                UpdateDialog();

                                pfnCallback(-1, extHw_Changed_SampleRate, 0, NULL);
                            }
                        }
                    }
                    return TRUE;
                }
                break;
                // Sampling mode combo
                case IDC_COMBO_SAMPLING_MODE:
                {
                    if (GET_WM_COMMAND_CMD(wParam, lParam) == CBN_SELCHANGE)
                    {
                        int mode = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));
                        int r = 0;
                        if (mode == 0)
                        {
                            r = fobos_rx_set_direct_sampling(gDev, 0);
                        }
                        else
                        {
                            r = fobos_rx_set_direct_sampling(gDev, 1);
                        }
                        if (giSamplingMode != mode)
                        {
                            if (pfnCallback)
                            {
                                pfnCallback(-1, extHw_Changed_LO, 0.0F, 0);
                            }
                        }
                        giSamplingMode = mode;
                        if (r != 0)
                            return FALSE;
                    }
                    return TRUE;
                }
                break;
                // Extrsnal clock check
                case IDC_CHECK_EXT_CLOCK:
                {
                    if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED)
                    {
                        if (IsDlgButtonChecked(hwndDlg, IDC_CHECK_EXT_CLOCK) == BST_CHECKED)
                        {
                            giExternalClock = 1;
                        }
                        else
                        {
                            giExternalClock = 0;
                        }
                        int r = fobos_rx_set_clk_source(gDev, giExternalClock);
                        if (r != 0)
                            return FALSE;
                    }
                    return TRUE;
                }
                break;

                // GPO checkbox clicked
                case IDC_CHECK_GPO0:
                case IDC_CHECK_GPO1:
                case IDC_CHECK_GPO2:
                case IDC_CHECK_GPO3:
                case IDC_CHECK_GPO4:
                case IDC_CHECK_GPO5:
                case IDC_CHECK_GPO6:
                case IDC_CHECK_GPO7:
                {
                    if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED)
                    {
                        uint32_t GPO_Bit = 1 << (CmdId - IDC_CHECK_GPO0);

                        if (IsDlgButtonChecked(hwndDlg, CmdId) == BST_CHECKED)
                        {
                            giUserGPO |= GPO_Bit;
                        }
                        else 
                        {
                            giUserGPO &= ~GPO_Bit;
                        }
                        int r = fobos_rx_set_user_gpo(gDev, giUserGPO);
                        if (r != 0)
                            return FALSE;
                    }
                    return TRUE;
                }
                break;
                /* Pressed Reset button */
                case IDC_BUTTON_CLOSE:
                {
                    if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED)
                    {

                        ShowWindow(ghDialog, SW_HIDE);

                        return TRUE;
                    }
                }
                break;
            }
        }
        break;
        /* Static text color message */
        case WM_CTLCOLORSTATIC:
        {
            /* Text color */
        }
        break;
        /* Closed dialog window */
        case WM_CLOSE:
        ShowWindow(ghDialog, SW_HIDE);
        return TRUE;
        break;
        /* Destroy dialog window */
        case WM_DESTROY:
        ShowWindow(ghDialog, SW_HIDE);
        ghDialog = NULL;
        return TRUE;
        break;
    }
    return false;
}
//==============================================================================
BOOL APIENTRY DllMain(HMODULE hModule,  DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
        hInst = hModule;
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
//==============================================================================
extern "C"
bool EXTIO_API InitHW(char *name, char *model, int& type)
{
#ifdef _DEBUG
    AllocConsole();
#endif
    type = exthwUSBfloat32;
    strcpy(name,  HWNAME);
    strcpy(model, HWMODEL);

    //AllocConsole();

    if ( !gbInitHW )
    {
        char lib_version[64];
        char drv_version[64];
        fobos_rx_get_api_info(lib_version, drv_version);
        // do initialization
        char serials[256];
        memset(serials, 0, sizeof(serials));
        giDeviceCount = fobos_rx_list_devices(serials);
        char * p_serials = serials;
        for (size_t i = 0; i < giDeviceCount; i++)
        {
            char * serial = strtok(p_serials, " ");
            if (serial)
            {
                strcpy(gSerials[i], serial);
            }
            else
            {
                break;
            }
        }

        if (!giDeviceCount)
        {
            MessageBox(NULL, TEXT("No Fobos SDR devices found"), TEXT("ExtIO Fobos SDR"), MB_ICONERROR | MB_OK);
            return false;
        }
        cprintf("found %d Fobos SDR devices\n", giDeviceCount);

        gbInitHW = true;
    }
    return gbInitHW;
}
//==============================================================================
extern "C"
bool EXTIO_API OpenHW(void)
{
    int r = fobos_rx_open(&gDev, giDeviceIdx);

    char hw_revision[32];
    char fw_version[32];
    char manufacturer[32];
    char product[32];
    char serial[32];
    fobos_rx_get_board_info(gDev,  hw_revision, fw_version, manufacturer, product, serial);
    sprintf(gBoardInfo, "hw: r.%s fw: v.%s", hw_revision, fw_version);
    if (r != 0) 
    {
        //MessageBox(NULL, TEXT("Open Error"),NULL, MB_OK);
        return false;
    }
    r = fobos_rx_set_samplerate(gDev, gSampleRates[giSrateIdx].value, 0);
    if (r != 0)
        return false;

    if (!gbChangeHW)
    {
        ghDialog = CreateDialog(hInst, MAKEINTRESOURCE(IDD_SETTINGS_DLG), NULL, (DLGPROC)MainDlgProc);
        ShowWindow(ghDialog, SW_HIDE);
    }

    return gbInitHW;
}
//==============================================================================
extern "C"
int  EXTIO_API StartHW(long LOfreq)
{
    int64_t ret = StartHW64( (int64_t)LOfreq );
    return (int)ret;
}
//==============================================================================
extern "C"
int EXTIO_API StartHW64(int64_t LOfreq)
{
    if (!gbInitHW)
        return -1;

    if (!gDev)
        return -1;

    SetHWLO64(LOfreq);

    if (StartThread() != 0)
        return -1;

    gbStartHW = true;

    // number of complex elements returned each
    // invocation of the callback routine
    return EXT_BLOCKLEN;
}
//==============================================================================
extern "C"
void EXTIO_API StopHW(void)
{
    StopThread();
    gbStartHW = false;
}
//==============================================================================
extern "C"
void EXTIO_API CloseHW(void)
{
    if (gbInitHW )
    {
        fobos_rx_close(gDev);
        gDev = NULL;
        if (!gbChangeHW)
        {
            if (ghDialog != NULL)
                DestroyWindow(ghDialog);
            gbInitHW = false;
        }
    }
}
//==============================================================================
extern "C"
int  EXTIO_API SetHWLO(long LOfreq)
{
    int64_t ret = SetHWLO64( (int64_t)LOfreq );
    return (ret & 0xFFFFFFFF);
}
//==============================================================================
extern "C"
int64_t EXTIO_API SetHWLO64(int64_t LOfreq)
{
    // ..... set here the LO frequency in the controlled hardware
    // Set here the frequency of the controlled hardware to LOfreq
    int64_t ret = LOfreq;

    // check limits
    if ((giSamplingMode == 0) || (giSamplingMode == 1))
    {
        if (LOfreq < LO_MIN)
        {
            ret = LO_MIN;
        }
        else if (LOfreq > LO_MAX)
        {
            ret = LO_MAX;
        }
        // take frequency
        if (ret != LOfreq)
        {
            if (pfnCallback)
            {
                pfnCallback(-1, extHw_Changed_LO, 0.0F, 0);
            }
        }
        gdLOfreq = ret;
        if (gbInitHW)
        {
            int r = fobos_rx_set_frequency(gDev, (double)LOfreq, 0);
            if (r != 0)
            {
                //MessageBox(NULL, TEXT("Set Freq Error!"),TEXT("Error!"), MB_OK|MB_ICONERROR);
            }
        }
    }
    else
    {
        ret = int64_t(gSampleRates[giSrateIdx].value) / 2;
        if (pfnCallback)
            pfnCallback(-1, extHw_Changed_LO, 0.0F, 0);
    }


    // 0 The function did complete without errors.
    // < 0 (a negative number N)
    //     The specified frequency  is  lower than the minimum that the hardware  is capable to generate.
    //     The absolute value of N indicates what is the minimum supported by the HW.
    // > 0 (a positive number N) The specified frequency is greater than the maximum that the hardware
    //     is capable to generate.
    //     The value of N indicates what is the maximum supported by the HW.
    return ret;
}
//==============================================================================
extern "C"
int  EXTIO_API GetStatus(void)
{
    return 0;  // status not supported by this specific HW,
}
//==============================================================================
extern "C"
void EXTIO_API SetCallback( pfnExtIOCallback funcptr )
{
    pfnCallback = funcptr;
    return;
}
//==============================================================================
extern "C"
long EXTIO_API GetHWLO(void)
{
    return (long)(GetHWLO64() & 0xFFFFFFFF);
}
//==============================================================================
extern "C"
int64_t EXTIO_API GetHWLO64(void)
{
    int64_t ret = (int64_t)gdLOfreq;
    if ((giSamplingMode == 0) || (giSamplingMode == 1))
    {
    }
    else
    {
        ret = int64_t(gSampleRates[giSrateIdx].value) / 2;
    }
    return ret;
}
//==============================================================================
extern "C"
long EXTIO_API GetHWSR(void)
{
    if (giSrateIdx >= 0 && giSrateIdx < (sizeof(gSampleRates) / sizeof(gSampleRates[0])))
    {
        return (long)gSampleRates[giSrateIdx].value;
    }
    return 0L;
}
//==============================================================================
extern "C"
void EXTIO_API VersionInfo(const char * progname, int ver_major, int ver_minor)
{
  SDR_progname[0] = 0;
  SDR_ver_major = -1;
  SDR_ver_minor = -1;

  if ( progname )
  {
    strncpy( SDR_progname, progname, sizeof(SDR_progname) -1 );
    SDR_ver_major = ver_major;
    SDR_ver_minor = ver_minor;

    // possibility to check program's capabilities
    // depending on SDR program name and version,
    // f.e. if specific extHWstatusT enums are supported
  }
}
//==============================================================================
extern "C"
int EXTIO_API ExtIoGetSrates( int srate_idx, double * samplerate )
{
    if (srate_idx < (sizeof(gSampleRates) / sizeof(gSampleRates[0])))
    {
        *samplerate = gSampleRates[srate_idx].value;
        return 0;
    }
    return 1;	// ERROR
}
//==============================================================================
extern "C"
int  EXTIO_API ExtIoGetActualSrateIdx(void)
{
    return giSrateIdx;
}
//==============================================================================
extern "C"
int  EXTIO_API ExtIoSetSrate( int srate_idx )
{
    if (srate_idx >= 0 && srate_idx < (sizeof(gSampleRates) / sizeof(gSampleRates[0])))
    {
        giSrateIdx = srate_idx;
        int r = fobos_rx_set_samplerate(gDev, gSampleRates[srate_idx].value, 0);
        if (r == 0)
        {
            pfnCallback(-1, extHw_Changed_SampleRate, 0.0F, 0);// Signal application
            return 0;
        }
    }
    return 1;	// ERROR
}
//==============================================================================
extern "C"
int  EXTIO_API ExtIoGetSetting(int idx, char * description, char * value)
{
    switch (idx)
    {
        case 0:
        snprintf(description, 1024, "%s", "Identifier");
        snprintf(value, 1024, "%s", SETTINGS_IDENTIFIER);
        return 0;

		case 1:
        snprintf(description, 1024, "%s", "SampleRateIdx");
        snprintf(value, 1024, "%d", giSrateIdx);
        return 0;
        
		case 2:
        snprintf(description, 1024, "%s", "SamplingMode");
        snprintf(value, 1024, "%d", giSamplingMode);
        return 0;
        
		case 3:
        snprintf(description, 1024, "%s", "ExternalClock");
        snprintf(value, 1024, "%d", giExternalClock);
        return 0;
        
		case 4:
        snprintf(description, 1024, "%s", "LNAGain");
        snprintf(value, 1024, "%d", giVgaGain);
        return 0;
        
		case 5:
        snprintf(description, 1024, "%s", "VGAGain");
        snprintf(value, 1024, "%d", giVgaGain);
        return 0;
        
		default:
        return -1; // ERROR
    }
    return -1; // ERROR
}
//==============================================================================
extern "C"
void EXTIO_API ExtIoSetSetting(int idx, const char * value)
{
    int tempInt;
    double tempDouble;
    // now we know that there's no need to save our settings into some (.ini) file,
    // what won't be possible without admin rights!!!,
    // if the program (and ExtIO) is installed in C:\Program files\..
    SDR_supports_settings = true;
    if (idx != 0 && !SDR_settings_valid)
        return;	// ignore settings for some other ExtIO

    switch (idx)
    {
        case 0:
        SDR_settings_valid = (value && !strcmp(value, SETTINGS_IDENTIFIER));
        // make identifier version specific??? - or not ==> never change order of idx!
        break;

        case 1:
        tempInt = atoi(value);
        if (tempInt <= 0) tempInt = 0;
        if (tempInt > (sizeof(gSampleRates) / sizeof(gSampleRates[0]) - 1)) tempInt = sizeof(gSampleRates) / sizeof(gSampleRates[0]) - 1;
        giSrateIdx = tempInt;
        break;

        case 2:
            giSamplingMode = atoi(value);
            giSamplingMode = 0;
        break;

        case 3:
        tempInt = atoi(value);
        if (tempInt > 0)
            giExternalClock = 1;
        else
            giExternalClock = 0;
        break;

        case 4:
        tempInt = atoi(value);
        if (tempInt >= 0 && tempInt <= 3)
            giLnaGain = tempInt;
        break;

        case 5:
        tempInt = atoi(value);
        if (tempInt >= 0 && tempInt <= 15)
            giVgaGain = tempInt;
        break;

        case 6:
        tempDouble = atof(value);
        if (tempDouble >= LO_MIN && tempDouble <= LO_MAX)
            gdLOfreq = tempDouble;
        break;
    }
}
//==============================================================================
extern "C"
void EXTIO_API ShowGUI(void)
{
    ShowWindow(ghDialog, SW_SHOW);
    SetForegroundWindow(ghDialog);
    return;
}
//==============================================================================
extern "C"
void EXTIO_API HideGUI(void)
{
    ShowWindow(ghDialog, SW_HIDE);
    return;
}
//==============================================================================
extern "C"
void EXTIO_API SwitchGUI(void)
{
    if (IsWindowVisible(ghDialog))
        ShowWindow(ghDialog, SW_HIDE);
    else
        ShowWindow(ghDialog, SW_SHOW);
    return;
}
//==============================================================================
