 
#define EXTIO_EXPORTS		1
#define HWNAME				"Fobos SDR"
#define HWMODEL				"Fobos SDR"
#define SETTINGS_IDENTIFIER	"Fobos SDR"
#define MAX_DEVICES         64
#define EXT_BLOCKLEN		(4096*8)

#include "ExtIO_FobosSDR.h"
#include "fobos.h"
#include "fobos_sdr.h"
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
#pragma comment(lib, "fobos_sdr.lib")
#define snprintf	_snprintf

static HMODULE hInst;

typedef struct sr 
{
    double value;
    const wchar_t* name;
} sr_t;

static double * gSampleRates;
static unsigned int gSampleRatesCount;

static bool SDR_supports_settings = false;  // assume not supported
static bool SDR_settings_valid = false;		// assume settings are for some other ExtIO

static char SDR_progname[32+1] = "\0";
static int  SDR_ver_major = -1;
static int  SDR_ver_minor = -1;

fobos_dev_t* fobos_dev = NULL;
fobos_sdr_dev_t * fobos_sdr_dev = NULL;
static int giDeviceCount = 0;
static int gSelectedIdx = 0;
static char gSerials[MAX_DEVICES][64];
static int gDevIdxx[MAX_DEVICES];
static int gDevTypes[MAX_DEVICES];
static int giStreaming;
static char gLibInfo[128];
static char gBoardInfo[128];

static HANDLE ghWorker = INVALID_HANDLE_VALUE;

static int64_t 	gdLOfreq = 100000000;
static int		giSrateIdx = 5; // 
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
void convert_samples(float* buf, uint32_t len)
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

            buf[i * 8 + 2] = -buf[i * 8 + 3]; // re = -im
            buf[i * 8 + 3] = 0.0f;             // im = 0

            buf[i * 8 + 4] = buf[i * 8 + 5];   // re = im
            buf[i * 8 + 5] = 0.0f;             // im = 0

            buf[i * 8 + 6] = -buf[i * 8 + 7]; // re = -im
            buf[i * 8 + 7] = 0.0f;             // im = 0
        }
    }
}
//==============================================================================
void fobos_callback(float* buf, uint32_t len, void* ctx)
{
    giStreaming = 1;
    if (pfnCallback)
    {
        convert_samples(buf, len);
        pfnCallback(EXT_BLOCKLEN, 0, 0.0F, buf);
    }
}
//==============================================================================
void fobos_sdr_callback(float* buf, uint32_t len, struct fobos_sdr_dev_t* sender, void * user)
{
    giStreaming = 1;
    if (pfnCallback)
    {
        convert_samples(buf, len);
        pfnCallback(EXT_BLOCKLEN, 0, 0.0F, buf);
    }
}
//==============================================================================
void UpdateDialog()
{
    if (ghDialog == nullptr) return;
    ComboBox_SetCurSel(GetDlgItem(ghDialog, IDC_COMBO_DEVICE), gSelectedIdx);// Device dropdown
    EnableWindow(GetDlgItem(ghDialog, IDC_COMBO_DEVICE), !giStreaming);
    ComboBox_SetCurSel(GetDlgItem(ghDialog, IDC_COMBO_SR), giSrateIdx);  // Samplerate dropdown
    EnableWindow(GetDlgItem(ghDialog, IDC_COMBO_SR), !giStreaming);

	SetWindowTextA(GetDlgItem(ghDialog, IDC_EDIT_API), gLibInfo);
    SetWindowTextA(GetDlgItem(ghDialog, IDC_EDIT_BOARD), gBoardInfo);
    SetWindowTextA(GetDlgItem(ghDialog, IDC_EDIT_SERIAL), gSerials[gSelectedIdx]);

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
void FillSampleRates(HWND hwndDlg)
{
	if (hwndDlg == nullptr) return;
	ComboBox_ResetContent(GetDlgItem(hwndDlg, IDC_COMBO_SR));
	for (int i = 0; i < gSampleRatesCount; i++)
	{
		wchar_t witem[64];
		wsprintf(witem, L"%d", (int)gSampleRates[i]);
		ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_SR), witem);
	}
}
//==============================================================================
unsigned int __stdcall ThreadProc(void* p)
{
    if (fobos_dev)
    {
        return fobos_rx_read_async(fobos_dev, fobos_callback, NULL, 16, EXT_BLOCKLEN);
    }
    if (fobos_sdr_dev)
    {
        return fobos_sdr_read_async(fobos_sdr_dev, fobos_sdr_callback, NULL, 16, EXT_BLOCKLEN);
    }
    return -1;
}
//==============================================================================
static int StartThread()
{
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
    
    int r = 0;
    if (fobos_dev)
    {
        r = fobos_rx_cancel_async(fobos_dev);
    }
    if (fobos_sdr_dev)
    {
        r = fobos_sdr_cancel_async(fobos_sdr_dev);
    }
    Sleep(500);
    WaitForSingleObject(ghWorker, INFINITE);
    CloseHandle(ghWorker);
    ghWorker = INVALID_HANDLE_VALUE;
    giStreaming = 0;
	cprintf("UpdateDialog..\n");
    UpdateDialog();
	cprintf("UpdateDialog..Ok\n");
    return r;
}
//==============================================================================
static INT_PTR CALLBACK MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    int r;
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            char info [128];
            sprintf(info, "build %s at %s", __DATE__, __TIME__);
            SetWindowTextA(GetDlgItem(hwndDlg, IDC_EDIT_EXTIO), info);

            for (int i = 0; i < giDeviceCount; i++)
            {
                sprintf(info, "Fobos SDR %s", gSerials[i]);
                wchar_t winfo[128];
                MultiByteToWideChar(CP_UTF8, 0, info, -1, winfo, 128);
                ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_DEVICE), winfo);
            }

			FillSampleRates(hwndDlg);

            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_SAMPLING_MODE), L"RF");
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_SAMPLING_MODE), L"IQ (HF1+HF2) direct sampling");
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_SAMPLING_MODE), L"HF1 direct sampling");
            ComboBox_AddString(GetDlgItem(hwndDlg, IDC_COMBO_SAMPLING_MODE), L"HF2 direct sampling");


            SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_LNA, TBM_SETRANGEMIN, FALSE, 0);
            SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_LNA, TBM_SETRANGEMAX, FALSE, 3);
            for (int i = 0; i <= 3; i++)
            {
                SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_LNA, TBM_SETTIC, FALSE, i);
            }

            SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_VGA, TBM_SETRANGEMIN, FALSE, 0);
            SendDlgItemMessage(hwndDlg, IDC_SLIDER_GAIN_VGA, TBM_SETRANGEMAX, FALSE, 31);
            for (int i = 0; i <= 31; i++)
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
                        if (fobos_dev)
                        {
                            r = fobos_rx_set_lna_gain(fobos_dev, giLnaGain);
                        }
                        if (fobos_sdr_dev)
                        {
                            r = fobos_sdr_set_lna_gain(fobos_sdr_dev, giLnaGain);
                        }
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

                    if (giVgaGain <= 31)
                    {
                        if (fobos_dev)
                        {
                            r = fobos_rx_set_vga_gain(fobos_dev, giVgaGain);
                        }
                        if (fobos_sdr_dev)
                        {
                            r = fobos_sdr_set_vga_gain(fobos_sdr_dev, giVgaGain);
                        }
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
                        if (gSelectedIdx != ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam)))
                        {
                            gbChangeHW = true;
                            int64_t freq = -1;
							if (gbInitHW)
							{
								if (gbStartHW)
								{
									freq = GetHWLO64();
									StopHW();
								}
								CloseHW();
							}
                            gSelectedIdx = ComboBox_GetCurSel(GET_WM_COMMAND_HWND(wParam, lParam));
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
                            if (sr >= 0 && sr < gSampleRatesCount)
                            {
                                giSrateIdx = sr;
                                if (fobos_dev)
                                {
                                    r = fobos_rx_set_samplerate(fobos_dev, gSampleRates[giSrateIdx], 0);
                                }
                                if (fobos_sdr_dev)
                                {
                                    r = fobos_sdr_set_samplerate(fobos_sdr_dev, gSampleRates[giSrateIdx]);
                                }
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
                        if (mode == 0)
                        {
                            if (fobos_dev)
                            {
                                r = fobos_rx_set_direct_sampling(fobos_dev, 0);
                            }
                            if (fobos_sdr_dev)
                            {
                                r = fobos_sdr_set_direct_sampling(fobos_sdr_dev, 0);
                            }
                        }
                        else
                        {
                            if (fobos_dev)
                            {
                                r = fobos_rx_set_direct_sampling(fobos_dev, 1);
                            }
                            if (fobos_sdr_dev)
                            {
                                r = fobos_sdr_set_direct_sampling(fobos_sdr_dev, 1);
                            }
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
                        if (fobos_dev)
                        {
                            r = fobos_rx_set_clk_source(fobos_dev, giExternalClock);
                        }
                        if (fobos_sdr_dev)
                        {
                            r = fobos_sdr_set_clk_source(fobos_sdr_dev, giExternalClock);
                        }
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
                        if (fobos_dev)
                        {
                            r = fobos_rx_set_user_gpo(fobos_dev, giUserGPO);
                        }
                        if (fobos_sdr_dev)
                        {
                            r = fobos_sdr_set_user_gpo(fobos_sdr_dev, giUserGPO);
                        }
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
        // do initialization
        char serials[256];
        memset(serials, 0, sizeof(serials));
        
        int count = fobos_rx_list_devices(serials);
        char * p_serials = serials;
        size_t s = 0;
        for (size_t i = 0; i < count; i++)
        {
            char * serial = strtok(p_serials, " ");
            if (serial)
            {
                strcpy(gSerials[s], serial);
                gDevIdxx[s] = i;
                gDevTypes[s] = 0;
                s++;
            }
            else
            {
                break;
            }
        }
		memset(serials, 0, sizeof(serials));
        count = fobos_sdr_list_devices(serials);
        p_serials = serials;
        for (size_t i = 0; i < count; i++)
        {
            char * serial = strtok(p_serials, " ");
            if (serial)
            {
                strcpy(gSerials[s], serial);
                strcat(gSerials[s], " (agile)");
                gDevIdxx[s] = i;
                gDevTypes[s] = 1;
                s++;
            }
            else
            {
                break;
            }
        }
        giDeviceCount = s;
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
    int r = 0;
    if (gDevTypes[gSelectedIdx] == 0)
    {
        r = fobos_rx_open(&fobos_dev, gDevIdxx[gSelectedIdx]);
    }
    else if (gDevTypes[gSelectedIdx] == 1)
    {
        r = fobos_sdr_open(&fobos_sdr_dev, gDevIdxx[gSelectedIdx]);
    }
    if (r != 0)
    {
        return false;
    }
    char hw_revision[64];
    char fw_version[64];
    char manufacturer[64];
    char product[64];
    char serial[64];
    char lib_version[64];
    char drv_version[64];
    if (fobos_dev)
    {
        r = fobos_rx_get_api_info(lib_version, drv_version);
        r = fobos_rx_get_board_info(fobos_dev, hw_revision, fw_version, manufacturer, product, serial);
    }
    if (fobos_sdr_dev)
    {
        r = fobos_sdr_get_api_info(lib_version, drv_version);
        r = fobos_sdr_get_board_info(fobos_sdr_dev, hw_revision, fw_version, manufacturer, product, serial);
    }
    if (r != 0)
    {
        return false;
    }
    sprintf(gLibInfo, "lib v.%s drv %s", lib_version, drv_version);
    sprintf(gBoardInfo, "hw: r.%s fw: v.%s", hw_revision, fw_version);
    if (fobos_dev)
    {
        r = fobos_rx_get_samplerates(fobos_dev, 0, &gSampleRatesCount);
        delete gSampleRates;
        gSampleRates = new double[gSampleRatesCount];
        r = fobos_rx_get_samplerates(fobos_dev, gSampleRates, &gSampleRatesCount);
    }
    if (fobos_sdr_dev)
    {
        r = fobos_sdr_get_samplerates(fobos_sdr_dev, 0, &gSampleRatesCount);
        delete gSampleRates;
        gSampleRates = new double[gSampleRatesCount];
        r = fobos_sdr_get_samplerates(fobos_sdr_dev, gSampleRates, 0);
    }
	FillSampleRates(ghDialog);
    if (fobos_dev)
    {
        r = fobos_rx_set_samplerate(fobos_dev, gSampleRates[giSrateIdx], 0);
    }
    if (fobos_sdr_dev)
    {
        r = fobos_sdr_set_samplerate(fobos_sdr_dev, gSampleRates[giSrateIdx]);
    }
    if (r != 0)
    {
        return false;
    }
    if (fobos_sdr_dev)
    {
        r = fobos_sdr_set_auto_bandwidth(fobos_sdr_dev, 0.9);
    }
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
    if (LOfreq < 0)
        LOfreq += 4294967296LL;
    if (!gbInitHW)
        return -1;

    if ((!fobos_dev) && (!fobos_sdr_dev))
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
    if (gbInitHW)
    {
        if (fobos_dev)
        {
            fobos_rx_close(fobos_dev);
        }
        fobos_dev = NULL;
        if (fobos_sdr_dev)
        {
            fobos_sdr_close(fobos_sdr_dev);
        }
        fobos_sdr_dev = NULL;
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
            int r = 0;
            if (fobos_dev)
            {
                r = fobos_rx_set_frequency(fobos_dev, (double)LOfreq, 0);
            }
            if (fobos_sdr_dev)
            {
                r = fobos_sdr_set_frequency(fobos_sdr_dev, (double)LOfreq);
            }
            if (r != 0)
            {
                //MessageBox(NULL, TEXT("Set Freq Error!"),TEXT("Error!"), MB_OK|MB_ICONERROR);
            }
        }
    }
    else
    {
        ret = int64_t(gSampleRates[giSrateIdx]) / 2;
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
        ret = int64_t(gSampleRates[giSrateIdx]) / 2;
    }
    return ret;
}
//==============================================================================
extern "C"
long EXTIO_API GetHWSR(void)
{
    if (giSrateIdx < 0) giSrateIdx = 0;
    if (giSrateIdx >= gSampleRatesCount) giSrateIdx = gSampleRatesCount - 1;
    return (long)gSampleRates[giSrateIdx];
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
int EXTIO_API ExtIoGetSrates(int srate_idx, double * samplerate)
{
    if (srate_idx < (gSampleRatesCount))
    {
        *samplerate = gSampleRates[srate_idx];
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
    if (srate_idx >= 0 && srate_idx < (gSampleRatesCount))
    {
        giSrateIdx = srate_idx;
        int r = 0;
        if (fobos_dev)
        {
            r = fobos_rx_set_samplerate(fobos_dev, gSampleRates[srate_idx], 0);
        }
        if (fobos_sdr_dev)
        {
            r = fobos_sdr_set_samplerate(fobos_sdr_dev, gSampleRates[srate_idx]);
        }
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
        if (gSampleRatesCount > 0)
        {
            if (tempInt > (gSampleRatesCount - 1)) tempInt = gSampleRatesCount - 1;
        }
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
        if (tempInt >= 0 && tempInt <= 31)
            giVgaGain = tempInt;
        break;

        case 6:
        tempDouble = atof(value);
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
