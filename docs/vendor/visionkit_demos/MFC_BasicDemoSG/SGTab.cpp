#include "stdafx.h"
#include "BasicDemoSG.h"
#include "SGTab.h"
#include "afxdialogex.h"
#include "BasicDemoSGDlg.h"  // 添加父窗口头文件
#include "VkCamera.h"        // 添加VkCamera头文件
#include <vector>


CSGTab::CSGTab(CWnd* pParent)
	: CDialogEx(IDD_SG_TAB, pParent)
{

}


CSGTab::~CSGTab()
{
}

void CSGTab::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);	
	DDX_Control(pDX, IDC_CL_CONFIG_COMBO, m_boxCLConfig);
	DDX_Control(pDX, IDC_CAM_COLOR_TYPE_COMBO, m_boxCamColorType);
	DDX_Control(pDX, IDC_CAM_PIXEL_DEPTH_COMBO, m_boxCamPixelDepth);
	DDX_Control(pDX, IDC_CAM_SCAN_TYPE_COMBO, m_boxCamScanType);
	DDX_Control(pDX, IDC_DEV_TAP_GEOMETRY_COMBO, m_boxDevTapGeometry);
	DDX_Control(pDX, IDC_HORIZONTAL_EDIT, m_editHorizontal);
	DDX_Control(pDX, IDC_VERTICAL_EDIT, m_editVertical);

	DDX_Control(pDX, IDC_CC1_COMBO, m_boxCC1);
	DDX_Control(pDX, IDC_LINE_TRI_ENABLE_COMBO, m_boxLineTriEnable);
	DDX_Control(pDX, IDC_LINE_TRI_SOURCE_COMBO, m_boxLineTriSource);
	DDX_Control(pDX, IDC_LINE_TRI_RATE_EDIT, m_editLineTriRate);
	DDX_Control(pDX, IDC_LINE_TRI_SOFTWARE_BUTTON, m_btnLineTriSoftware);
	DDX_Control(pDX, IDC_FRAME_TRI_ENABLE_COMBO, m_boxFrameTriEnable);
	DDX_Control(pDX, IDC_FRAME_TRI_SOURCE_COMBO, m_boxFrameTriSource);
	DDX_Control(pDX, IDC_FRAME_TRI_RATE_EDIT, m_editFrameTriRate);
	DDX_Control(pDX, IDC_FRAME_TRI_SOFTWARE_BUTTON, m_btnFrameTriSoftware);
}

BEGIN_MESSAGE_MAP(CSGTab, CDialogEx)
	ON_BN_CLICKED(IDC_START_GRAB_BUTTON, &CSGTab::OnBnClickedStartGrabButton)
	ON_BN_CLICKED(IDC_STOP_GRAB_BUTTON, &CSGTab::OnBnClickedStopGrabButton)
	ON_BN_CLICKED(IDC_SAVE_RAW_BUTTON, &CSGTab::OnBnClickedSaveRawButton)
	ON_BN_CLICKED(IDC_SAVE_BMP_BUTTON, &CSGTab::OnBnClickedSaveBmpButton)
	ON_BN_CLICKED(IDC_SAVE_JPEG_BUTTON, &CSGTab::OnBnClickedSaveJpegButton)
	ON_BN_CLICKED(IDC_SAVE_TIF_BUTTON, &CSGTab::OnBnClickedSaveTifButton)
	ON_BN_CLICKED(IDC_SAVE_PNG_BUTTON, &CSGTab::OnBnClickedSavePngButton)
	ON_BN_CLICKED(IDC_SG_GET_PARAM_BUTTON, &CSGTab::OnBnClickedSgGetParamButton)
	ON_CBN_SELCHANGE(IDC_CL_CONFIG_COMBO, &CSGTab::OnCbnSelchangeClConfigCombo)
	ON_CBN_SELCHANGE(IDC_CAM_COLOR_TYPE_COMBO, &CSGTab::OnCbnSelchangeCamColorTypeCombo)
	ON_CBN_SELCHANGE(IDC_CAM_PIXEL_DEPTH_COMBO, &CSGTab::OnCbnSelchangeCamPixelDepthCombo)
	ON_CBN_SELCHANGE(IDC_CAM_SCAN_TYPE_COMBO, &CSGTab::OnCbnSelchangeCamScanTypeCombo)
	ON_CBN_SELCHANGE(IDC_DEV_TAP_GEOMETRY_COMBO, &CSGTab::OnCbnSelchangeDevTapGeometryCombo)
	ON_CBN_SELCHANGE(IDC_CC1_COMBO, &CSGTab::OnCbnSelchangeCc1Combo)
	ON_CBN_SELCHANGE(IDC_LINE_TRI_ENABLE_COMBO, &CSGTab::OnCbnSelchangeLineTriEnableCombo)
	ON_CBN_SELCHANGE(IDC_LINE_TRI_SOURCE_COMBO, &CSGTab::OnCbnSelchangeLineTriSourceCombo)
	ON_CBN_SELCHANGE(IDC_FRAME_TRI_ENABLE_COMBO, &CSGTab::OnCbnSelchangeFrameTriEnableCombo)
	ON_CBN_SELCHANGE(IDC_FRAME_TRI_SOURCE_COMBO, &CSGTab::OnCbnSelchangeFrameTriSourceCombo)
	ON_BN_CLICKED(IDC_LINE_TRI_SOFTWARE_BUTTON, &CSGTab::OnBnClickedLineTriSoftwareButton)
	ON_BN_CLICKED(IDC_FRAME_TRI_SOFTWARE_BUTTON, &CSGTab::OnBnClickedFrameTriSoftwareButton)
	ON_BN_CLICKED(IDC_SG_CONFIG_SAVE_BUTTON, &CSGTab::OnBnClickedSgConfigSaveButton)
	ON_BN_CLICKED(IDC_SG_CONFIG_LOAD_BUTTON, &CSGTab::OnBnClickedSgConfigLoadButton)
END_MESSAGE_MAP()

BOOL CSGTab::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	m_pParentDlg = nullptr;
	m_pSGCardController = nullptr;

	return TRUE;
}

void CSGTab::OnBnClickedStartGrabButton()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	// Cn:开启抓图 | En:Start Grabbing
	int ret = m_pSGCardController->StartImageAcquisition(1000, 0, m_pParentDlg);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to start grabbing"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}

	::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_UPDATE_UI, 0, 0);
}

void CSGTab::OnBnClickedStopGrabButton()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	// Cn:停止抓图 | En:Stop Grabbing
	int ret = m_pSGCardController->StopImageAcquisition(0);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to stop grabbing"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}

	::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_UPDATE_UI, 0, 0);
}

void CSGTab::OnBnClickedSaveRawButton()
{
	if (!m_pParentDlg) {
        CString* pMsg = new CString(_T("Unable to find main window"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		return;
	}

	::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SG_SAVE_RAW, 0, 0);
}

void CSGTab::OnBnClickedSaveBmpButton()
{
	if (!m_pParentDlg) {
        CString* pMsg = new CString(_T("Unable to find main window"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		return;
	}

	::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SG_SAVE_BMP, 0, 0);
}

void CSGTab::OnBnClickedSaveJpegButton()
{
	if (!m_pParentDlg) {
        CString* pMsg = new CString(_T("Unable to find main window"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		return;
	}

	::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SG_SAVE_JPEG, 0, 0);
}

void CSGTab::OnBnClickedSaveTifButton()
{
	if (!m_pParentDlg) {
        CString* pMsg = new CString(_T("Unable to find main window"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		return;
	}

	::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SG_SAVE_TIF, 0, 0);
}

void CSGTab::OnBnClickedSavePngButton()
{
	if (!m_pParentDlg) {
        CString* pMsg = new CString(_T("Unable to find main window"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		return;
	}

	::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SG_SAVE_PNG, 0, 0);
}

void CSGTab::OnCbnSelchangeClConfigCombo()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CString strSelected;
	m_boxCLConfig.GetLBText(m_boxCLConfig.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCardController->SetEnumValue("ClConfiguration", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Camera Link Config"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnCbnSelchangeCamColorTypeCombo()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CString strSelected;
	m_boxCamColorType.GetLBText(m_boxCamColorType.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCardController->SetEnumValue("CameraColorType", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Camera Color Type"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnCbnSelchangeCamPixelDepthCombo()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CString strSelected;
	m_boxCamPixelDepth.GetLBText(m_boxCamPixelDepth.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCardController->SetEnumValue("CameraPixelDepth", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Camera Pixel Depth"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnCbnSelchangeCamScanTypeCombo()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CString strSelected;
	m_boxCamScanType.GetLBText(m_boxCamScanType.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCardController->SetEnumValue("CameraScanType", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Camera Scan Type"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnCbnSelchangeDevTapGeometryCombo()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CString strSelected;
	m_boxDevTapGeometry.GetLBText(m_boxDevTapGeometry.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCardController->SetEnumValue("DeviceTapGeometry", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Device Tap Geometry"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnCbnSelchangeCc1Combo()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CString strSelected;
	m_boxCC1.GetLBText(m_boxCC1.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCardController->SetEnumValue("CL_CC1", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set CC1"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnCbnSelchangeLineTriEnableCombo()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CString strSelected;
	m_boxLineTriEnable.GetLBText(m_boxLineTriEnable.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCardController->SetEnumValue("LineTriggerEnable", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Line Trigger Enable"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnCbnSelchangeLineTriSourceCombo()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CString strSelected;
	m_boxLineTriSource.GetLBText(m_boxLineTriSource.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCardController->SetEnumValue("LineTriggerSource", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Line Trigger Source"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnCbnSelchangeFrameTriEnableCombo()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CString strSelected;
	m_boxFrameTriEnable.GetLBText(m_boxFrameTriEnable.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCardController->SetEnumValue("FrameTriggerEnable", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Frame Trigger Enable"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnCbnSelchangeFrameTriSourceCombo()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CString strSelected;
	m_boxFrameTriSource.GetLBText(m_boxFrameTriSource.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pSGCardController->SetEnumValue("FrameTriggerSource", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Frame Trigger Source"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnBnClickedLineTriSoftwareButton()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	int ret = m_pSGCardController->ExecuteCommand("LineTriggerSoftware");
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to execute Line Trigger Software command"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnBnClickedFrameTriSoftwareButton()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	int ret = m_pSGCardController->ExecuteCommand("FrameTriggerSoftware");
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to execute Frame Trigger Software command"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnBnClickedSgGetParamButton()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	// Cn:设置所有枚举型参数 | En:Set all enumeration parameters
    setupEnumComboBox("ClConfiguration", &m_boxCLConfig, CString("Failed to get Camera Link Config"));
    setupEnumComboBox("CameraColorType", &m_boxCamColorType, CString("Failed to get Camera Color Type"));
    setupEnumComboBox("CameraPixelDepth", &m_boxCamPixelDepth, CString("Failed to get Camera Pixel Depth"));
    setupEnumComboBox("CameraScanType", &m_boxCamScanType, CString("Failed to get Camera Scan Type"));
    setupEnumComboBox("DeviceTapGeometry", &m_boxDevTapGeometry, CString("Failed to get Device Tap Geometry"));
    setupEnumComboBox("CL_CC1", &m_boxCC1, CString("Failed to get CC1"));
    setupEnumComboBox("LineTriggerSource", &m_boxLineTriSource, CString("Failed to get Line Trigger Source"));
    setupEnumComboBox("FrameTriggerSource", &m_boxFrameTriSource, CString("Failed to get Frame Trigger Source"));
    setupEnumComboBox("LineTriggerEnable", &m_boxLineTriEnable, CString("Failed to get Line Trigger Enable"));
    setupEnumComboBox("FrameTriggerEnable", &m_boxFrameTriEnable, CString("Failed to get Frame Trigger Enable"));

	// Cn:获取Int型参数 | En:Get Int type parameters
	unsigned long long nWidth;
	int ret = m_pSGCardController->GetIntValue("CameraWidth", &nWidth);
    if (ret != OK)
    {
		m_editHorizontal.Clear();
        CString* pMsg = new CString(_T("Failed to get Horizontal Active"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
    else
    {
		CString strWidth;
		strWidth.Format(_T("%llu"), nWidth);
		m_editHorizontal.SetWindowText(strWidth);
	}

	// Cn:获取Int型参数 | En:Get Int type parameters
	unsigned long long nHeight;
	ret = m_pSGCardController->GetIntValue("CameraHeight", &nHeight);
    if (ret != OK)
    {
		m_editVertical.Clear();
        CString* pMsg = new CString(_T("Failed to get Vertical Active"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
    else
    {
		CString strHeight;
		strHeight.Format(_T("%llu"), nHeight);
		m_editVertical.SetWindowText(strHeight);
	}

	// Cn:获取Double型参数 | En:Get Double type parameters
	double dLineTriRate;
	ret = m_pSGCardController->GetDoubleValue("LineInternalTriggerRate", &dLineTriRate);
    if (ret != OK)
    {
		m_editLineTriRate.Clear();
        CString* pMsg = new CString(_T("Failed to get Internal Line Trigger Actual Freq (Hz)"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
    else
    {
		CString strLineTriRate;
		strLineTriRate.Format(_T("%.2f"), dLineTriRate);
		m_editLineTriRate.SetWindowText(strLineTriRate);
	}

	// Cn:获取Double型参数 | En:Get Double type parameters
	double dFrameTriRate;
	ret = m_pSGCardController->GetDoubleValue("FrameInternalTriggerRate", &dFrameTriRate);
    if (ret != OK)
    {
		m_editFrameTriRate.Clear();
        CString* pMsg = new CString(_T("Failed to get Internal Frame Trigger Actual Freq (Hz)"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
    else
    {
		CString strFrameTriRate;
		strFrameTriRate.Format(_T("%.2f"), dFrameTriRate);
		m_editFrameTriRate.SetWindowText(strFrameTriRate);
	}
}

void CSGTab::OnBnClickedSgConfigSaveButton()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CFileDialog fileDlg(
		FALSE,
		_T("cfg"),
		_T("SGFeatureFile.cfg"),
		OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR,
		_T("Configuration Files (*.cfg)|*.cfg|All Files (*.*)|*.*||"), this);

	fileDlg.m_ofn.lpstrTitle = _T("Save SG Card Configuration File");

	if (fileDlg.DoModal() != IDOK)
	{
		return;
	}

	CString strFilePath = fileDlg.GetPathName();

	// Cn:保存采集卡配置文件到本地 | En:Save SG Card Configuration File to Local
	int ret = m_pSGCardController->ConfigSave(CT2A(strFilePath));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to save SG card configuration file"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
    else
    {
		CString strMsg;
        strMsg.Format(_T("SG card configuration has been successfully saved to:\n%s"), strFilePath);
		AfxMessageBox(strMsg, MB_OK | MB_ICONINFORMATION);
	}
}

void CSGTab::OnBnClickedSgConfigLoadButton()
{
	if (!m_pSGCardController || !m_pParentDlg) return;

	CFileDialog fileDlg(
		TRUE,
		_T("cfg"),
		_T("SGFeatureFile.cfg"),
		OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR,
		_T("Configuration Files (*.cfg)|*.cfg|All Files (*.*)|*.*||"), this);

	fileDlg.m_ofn.lpstrTitle = _T("Load SG Card Configuration File");

	if (fileDlg.DoModal() != IDOK)
	{
		return;
	}

	CString strFilePath = fileDlg.GetPathName();

	// Cn:加载采集卡配置文件到设备 | En:Load SG Card Configuration file to device
	int ret = m_pSGCardController->ConfigLoad(CT2A(strFilePath));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to load SG card configuration file"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
	else
	{
		CString strMsg;
        strMsg.Format(_T("SG card configuration has been loaded from file:\n%s"), strFilePath);
		AfxMessageBox(strMsg, MB_OK | MB_ICONINFORMATION);
	}
}

void CSGTab::OnApplyHorizontal()
{
	if (!m_pSGCardController || !m_pParentDlg) return;
	CString strValue;
	m_editHorizontal.GetWindowText(strValue);

    if (strValue.IsEmpty())
    {
        CString* pMsg = new CString(_T("Please enter Horizontal Active value"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		m_editHorizontal.SetFocus();
		return;
	}

	int nHorizontal = _ttoi(strValue);
	// Cn:设置Int型参数 | En:Set Int type parameters
	int ret = m_pSGCardController->SetIntValue("CameraWidth", nHorizontal);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Horizontal Active"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnApplyVertical()
{
	if (!m_pSGCardController || !m_pParentDlg) return;
	CString strValue;
	m_editVertical.GetWindowText(strValue);

    if (strValue.IsEmpty())
    {
        CString* pMsg = new CString(_T("Please enter Vertical Active value"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		m_editVertical.SetFocus();
		return;
	}

	int nVertical = _ttoi(strValue);
	// Cn:设置Int型参数 | En:Set Int type parameters
	int ret = m_pSGCardController->SetIntValue("CameraHeight", nVertical);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Vertical Active"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnApplyLineTriRate()
{
	if (!m_pSGCardController || !m_pParentDlg) return;
	CString strValue;
	m_editLineTriRate.GetWindowText(strValue);

	strValue.Trim();
    if (strValue.IsEmpty())
    {
        CString* pMsg = new CString(_T("Please enter Internal Line Trigger Actual Freq (Hz) value"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		
		m_editLineTriRate.SetFocus();
		return;
	}

	double dLineTriRate = 0.0;
	_stscanf_s(strValue, _T("%lf"), &dLineTriRate);
	// Cn:设置Double型参数 | En:Set Double type parameters
	int ret = m_pSGCardController->SetDoubleValue("LineInternalTriggerRate", dLineTriRate);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Internal Line Trigger Actual Freq (Hz)"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CSGTab::OnApplyFrameTriRate()
{
	if (!m_pSGCardController || !m_pParentDlg) return;
	CString strValue;
	m_editFrameTriRate.GetWindowText(strValue);

	strValue.Trim();
    if (strValue.IsEmpty())
    {
        CString* pMsg = new CString(_T("Please enter Internal Frame Trigger Actual Freq (Hz) value"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));

		m_editFrameTriRate.SetFocus();
		return;
	}

	double dFrameTriRate = 0.0;
	_stscanf_s(strValue, _T("%lf"), &dFrameTriRate);
	// Cn:设置Double型参数 | En:Set Double type parameters
	int ret = m_pSGCardController->SetDoubleValue("FrameInternalTriggerRate", dFrameTriRate);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Internal Frame Trigger Actual Freq (Hz)"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

BOOL CSGTab::PreTranslateMessage(MSG* pMsg)
{
	if (pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_RETURN) {
		if (GetFocus() == GetDlgItem(IDC_HORIZONTAL_EDIT)) {
			OnApplyHorizontal();
			return TRUE;
		}
		else if (GetFocus() == GetDlgItem(IDC_VERTICAL_EDIT)) {
			OnApplyVertical();
			return TRUE;
		}
		else if (GetFocus() == GetDlgItem(IDC_LINE_TRI_RATE_EDIT)) {
			OnApplyLineTriRate();
			return TRUE;
		}
		else if (GetFocus() == GetDlgItem(IDC_FRAME_TRI_RATE_EDIT)) {
			OnApplyFrameTriRate();
			return TRUE;
		}
	}
	return CDialogEx::PreTranslateMessage(pMsg);
}

void CSGTab::setupEnumComboBox(const char* feature, CComboBox* box, const CString& errorMsg) {
	// Cn:获取枚举项数量 | En:Get the number of enumeration items
	int enumCnt = 0;
	int ret = m_pSGCardController->GetFeatureEnumArray(feature, nullptr, &enumCnt);
	if (ret != OK || enumCnt < 0) {
		CString* pMsg = new CString(errorMsg);
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
		return;
	}

	// Cn:获取枚举项 | En:Get enumeration items
	std::vector<ENUMARRAY> enumData(enumCnt);
	ret = m_pSGCardController->GetFeatureEnumArray(feature, enumData.data(), &enumCnt);
	if (ret != OK) {
		CString* pMsg = new CString(errorMsg);
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
		return;
	}

	box->ResetContent();
	for (const auto& item : enumData) {
		box->AddString(CString(item.entry));
	}

	// Cn:获取当前值并设置选中项 | En:Get the current value and set the selected item
	char value[256];
	ret = m_pSGCardController->GetEnumValue(feature, value);
	if (ret != OK) {
		CString* pMsg = new CString(errorMsg);
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
	else {
		CString strValue(value);
		int index = box->FindStringExact(-1, strValue);
		if (index != CB_ERR) {
			box->SetCurSel(index);
		}
	}
}
