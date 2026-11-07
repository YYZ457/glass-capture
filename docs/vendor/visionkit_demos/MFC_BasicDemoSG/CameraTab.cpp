#include "stdafx.h"
#include "BasicDemoSG.h"
#include "CameraTab.h"
#include "afxdialogex.h"
#include "BasicDemoSGDlg.h" 
#include "VkCamera.h"
#include <vector>


CCameraTab::CCameraTab(CWnd* pParent)
	: CDialogEx(IDD_CAMERA_TAB, pParent)
{
}

CCameraTab::~CCameraTab()
{
}

void CCameraTab::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_EXPO_EDIT, m_editExpo);
	DDX_Control(pDX, IDC_GAIN_EDIT, m_editGain);
	DDX_Control(pDX, IDC_FRAME_RATE_EDIT, m_editFrameRate);
	DDX_Control(pDX, IDC_PIXEL_FORMAT_COMBO, m_boxPixelFormat);
	DDX_Control(pDX, IDC_TRIGGER_MODE_COMBO, m_boxTriggerMode);
}

BEGIN_MESSAGE_MAP(CCameraTab, CDialogEx)
	ON_CBN_SELCHANGE(IDC_PIXEL_FORMAT_COMBO, &CCameraTab::OnCbnSelchangePixelFormatCombo)
	ON_CBN_SELCHANGE(IDC_TRIGGER_MODE_COMBO, &CCameraTab::OnCbnSelchangeTriggerModeCombo)
	ON_BN_CLICKED(IDC_CAMERA_GET_PARAM_BUTTON, &CCameraTab::OnBnClickedCameraGetParamButton)
	ON_BN_CLICKED(IDC_CAMERA_CONFIG_SAVE_BUTTON, &CCameraTab::OnBnClickedCameraConfigSaveButton)
	ON_BN_CLICKED(IDC_CAMERA_CONFIG_LOAD_BUTTON, &CCameraTab::OnBnClickedCameraConfigLoadButton)
END_MESSAGE_MAP()

BOOL CCameraTab::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	m_pParentDlg = nullptr;
	m_pCameraController = nullptr;

	return TRUE;
}

void CCameraTab::OnCbnSelchangePixelFormatCombo()
{
	if (!m_pCameraController || !m_pParentDlg) return;

	CString strSelected;
	m_boxPixelFormat.GetLBText(m_boxPixelFormat.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pCameraController->SetEnumValue("PixelFormat", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Pixel Format"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CCameraTab::OnCbnSelchangeTriggerModeCombo()
{
	if (!m_pCameraController || !m_pParentDlg) return;

	CString strSelected;
	m_boxTriggerMode.GetLBText(m_boxTriggerMode.GetCurSel(), strSelected);

	// Cn:设置Enum型参数 | En:Set Enum type parameters
	int ret = m_pCameraController->SetEnumValue("TriggerMode", CT2A(strSelected));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Trigger Mode"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CCameraTab::OnBnClickedCameraGetParamButton()
{
	if (!m_pCameraController || !m_pParentDlg) return;

	// Cn:设置所有枚举型参数 | En:Set all enumeration parameters
    setupEnumComboBox("PixelFormat", &m_boxPixelFormat, CString("Failed to get Pixel Format"));
    setupEnumComboBox("TriggerMode", &m_boxTriggerMode, CString("Failed to get Trigger Mode"));

	// Cn:获取Int型参数 | En:Get Int type parameters
	unsigned long long nExposureTime = 0;
	int ret = m_pCameraController->GetIntValue("ExposureTime", &nExposureTime);
    if (ret != OK)
    {
		m_editExpo.Clear();
        CString* pMsg = new CString(_T("Failed to get Exposure Time"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
    else
    {
		CString strExpo;
		strExpo.Format(_T("%llu"), nExposureTime);
		m_editExpo.SetWindowText(strExpo);
	}

	// Cn:获取Double型参数 | En:Get Double type parameters
	double dGain = 0.0;
	ret = m_pCameraController->GetDoubleValue("DigitalGain", &dGain);
    if (ret != OK)
    {
		m_editGain.Clear();
        CString* pMsg = new CString(_T("Failed to get Digital Gain"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
    else
    {
		CString strGain;
		strGain.Format(_T("%.2f"), dGain); // 保留2位小数
		m_editGain.SetWindowText(strGain);
	}

	// Cn:获取Int型参数 | En:Get Int type parameters
	unsigned long long nFrameRate = 0;
	ret = m_pCameraController->GetIntValue("FrameRate", &nFrameRate);
    if (ret != OK)
    {
		m_editFrameRate.Clear();
        CString* pMsg = new CString(_T("Failed to get Frame Rate"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
    else
    {
		CString strFrameRate;
		strFrameRate.Format(_T("%llu"), nFrameRate);
		m_editFrameRate.SetWindowText(strFrameRate);
	}
}

void CCameraTab::OnBnClickedCameraConfigSaveButton()
{
	if (!m_pCameraController || !m_pParentDlg) return;

	CFileDialog fileDlg(
		FALSE,
		_T("cfg"),
		_T("CamFeatureFile.cfg"),
		OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR,
		_T("Configuration Files (*.cfg)|*.cfg|All Files (*.*)|*.*||"), this);

	fileDlg.m_ofn.lpstrTitle = _T("Save Camera Configuration File");

	if (fileDlg.DoModal() != IDOK)
	{
		return;
	}

	CString strFilePath = fileDlg.GetPathName();

	// Cn:保存相机配置文件到本地 | En:Save camera configuration file to local
	int ret = m_pCameraController->ConfigSave(CT2A(strFilePath));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to save camera configuration file"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
    else
    {
		CString strMsg;
        strMsg.Format(_T("Camera configuration has been successfully saved to:\n%s"), strFilePath);
		AfxMessageBox(strMsg, MB_OK | MB_ICONINFORMATION);
	}
}

void CCameraTab::OnBnClickedCameraConfigLoadButton()
{
	if (!m_pCameraController || !m_pParentDlg) return;

	CFileDialog fileDlg(
		TRUE,
		_T("cfg"),
		_T("CamFeatureFile.cfg"),
		OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR,
		_T("Configuration Files (*.cfg)|*.cfg|All Files (*.*)|*.*||"), this);

	fileDlg.m_ofn.lpstrTitle = _T("Load Camera Configuration File");

	if (fileDlg.DoModal() != IDOK)
	{
		return;
	}

	CString strFilePath = fileDlg.GetPathName();

	// Cn:加载相机配置文件到设备 | En:Load camera configuration file to device
	int ret = m_pCameraController->ConfigLoad(CT2A(strFilePath));
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to load camera configuration file"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
	else
	{
		CString strMsg;
        strMsg.Format(_T("Camera configuration has been loaded from file:\n%s"), strFilePath);
		AfxMessageBox(strMsg, MB_OK | MB_ICONINFORMATION);
	}
}

void CCameraTab::OnApplyExposure()
{
	if (!m_pCameraController || !m_pParentDlg) return;
	CString strValue;
	m_editExpo.GetWindowText(strValue);

    if (strValue.IsEmpty())
    {
        CString* pMsg = new CString(_T("Please enter Exposure Time value"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		m_editExpo.SetFocus();
		return;
	}

	int nExposureTime = _ttoi(strValue);
	// Cn:设置Int型参数 | En:Set Int type parameters
	int ret = m_pCameraController->SetIntValue("ExposureTime", nExposureTime);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Exposure Time"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CCameraTab::OnApplyGain() 
{
	if (!m_pCameraController || !m_pParentDlg) return;
	CString strValue;
	m_editGain.GetWindowText(strValue);

	strValue.Trim();
    if (strValue.IsEmpty())
    {
        CString* pMsg = new CString(_T("Please enter Gain value"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		m_editGain.SetFocus();
		return;
	}

	double dGain = 0.0;
	_stscanf_s(strValue, _T("%lf"), &dGain);
	// Cn:设置Double型参数 | En:Set Double type parameters
	int ret = m_pCameraController->SetDoubleValue("DigitalGain", dGain);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Gain"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

void CCameraTab::OnApplyFrameRate() 
{
	if (!m_pCameraController || !m_pParentDlg) return;
	CString strValue;
	m_editFrameRate.GetWindowText(strValue);

    if (strValue.IsEmpty())
    {
        CString* pMsg = new CString(_T("Please enter Frame Rate"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(-1));
		m_editFrameRate.SetFocus();
		return;
	}

	int nFrameRate = _ttoi(strValue);
	// Cn:设置Int型参数 | En:Set Int type parameters
	int ret = m_pCameraController->SetIntValue("AcquisitionFrameRate", nFrameRate);
    if (ret != OK)
    {
        CString* pMsg = new CString(_T("Failed to set Frame Rate"));
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
	}
}

BOOL CCameraTab::PreTranslateMessage(MSG* pMsg)
{
	if (pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_RETURN) {
		if (GetFocus() == GetDlgItem(IDC_EXPO_EDIT)) {
			OnApplyExposure();
			return TRUE;
		}
		else if (GetFocus() == GetDlgItem(IDC_GAIN_EDIT)) {
			OnApplyGain();
			return TRUE;
		}
		else if (GetFocus() == GetDlgItem(IDC_FRAME_RATE_EDIT)) {
			OnApplyFrameRate();
			return TRUE;
		}
	}
	return CDialogEx::PreTranslateMessage(pMsg);
}

void CCameraTab::setupEnumComboBox(const char* feature, CComboBox* box, const CString& errorMsg) {
	// Cn:获取枚举项数量 | En:Get the number of enumeration items
	int enumCnt = 0;
	int ret = m_pCameraController->GetFeatureEnumArray(feature, nullptr, &enumCnt);
	if (ret != OK || enumCnt < 0) {
		CString* pMsg = new CString(errorMsg);
		::PostMessage(m_pParentDlg->GetSafeHwnd(), WM_SHOW_ERROR_MSG, reinterpret_cast<WPARAM>(pMsg), static_cast<LPARAM>(ret));
		return;
	}

	// Cn:获取枚举项 | En:Get enumeration items
	std::vector<ENUMARRAY> enumData(enumCnt);
	ret = m_pCameraController->GetFeatureEnumArray(feature, enumData.data(), &enumCnt);
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
	ret = m_pCameraController->GetEnumValue(feature, value);
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

