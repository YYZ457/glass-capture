#pragma once
#include "afxdialogex.h"

class CBasicDemoSGDlg;
class VkCamera;

class CCameraTab :
	public CDialogEx
{
public:
	CCameraTab(CWnd* pParent = nullptr);
	~CCameraTab();

	void SetParentDlg(CBasicDemoSGDlg* pDlg) { m_pParentDlg = pDlg; }							// Cn:设置父对话框引用 | En:Set parent dialog reference
	void SetDeviceController(VkCamera* pController) { m_pCameraController = pController; }		// Cn:注入相机控制接口 | En:Inject camera controller interface

	enum { IDD = IDD_CAMERA_TAB };
	
protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	DECLARE_MESSAGE_MAP()
	virtual BOOL OnInitDialog();

	/* Cn:控件事件处理 | En:Control Event Handling */
	/* Cn:参数获取与设置（相机） | En:Parameters Get and Set (Camera) */
	afx_msg void OnCbnSelchangePixelFormatCombo();			// Cn:设置像素格式参数 | En:Set Pixel Format Parameter
	afx_msg void OnCbnSelchangeTriggerModeCombo();			// Cn:设置触发模式参数 | En:Set Trigger Mode Parameter
	afx_msg void OnBnClickedCameraGetParamButton();			// Cn:获取相机参数 | En:Get Camera Parameters
	afx_msg void OnBnClickedCameraConfigSaveButton();		// Cn:保存相机配置参数文件 | En:Save Config File
	afx_msg void OnBnClickedCameraConfigLoadButton();		// Cn:加载相机配置参数文件 | En:Load Config File

	/* Cn:键盘事件处理 | En:Keyboard Event Handling */
	virtual BOOL PreTranslateMessage(MSG* pMsg);
	void OnApplyExposure();
	void OnApplyGain();
	void OnApplyFrameRate();

private:
	CEdit				m_editExpo;         // Cn:曝光时间编辑框 | En:Exposure time edit box
	CEdit				m_editGain;         // Cn:增益编辑框 | En:Gain edit box
	CEdit				m_editFrameRate;    // Cn:帧率编辑框 | En:Frame rate edit box
	CComboBox			m_boxPixelFormat;	// Cn:像素格式参数下拉框 | En:Pixel Format parameter combo
	CComboBox			m_boxTriggerMode;	// Cn:触发模式参数下拉框 | En:Trigger Mode parameter combo

private:
	CBasicDemoSGDlg* m_pParentDlg;			// Cn:父对话框指针 | En:Parent dialog pointer
	VkCamera* m_pCameraController;			// Cn:相机控制接口（由父对话框注入） | En:Camera control interface (injected by parent dialog)

private:
	void setupEnumComboBox(const char* feature,
		CComboBox* box, const CString& errorMsg);				// Cn:配置ComboBox控件子项 | En:Configuring ComboBox control sub-items
};

