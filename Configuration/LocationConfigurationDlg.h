#pragma once

#include "../Common/Common.h"
#include "SystemConfigurationPage.h"

// CLocationConfigurationDlg dialog
namespace ConfigurationDialog
{

	class CLocationConfigurationDlg : public CSystemConfigurationPage
	{
		DECLARE_DYNAMIC(CLocationConfigurationDlg)

	public:
		CLocationConfigurationDlg();
		virtual ~CLocationConfigurationDlg();

	// Dialog Data
		enum { IDD = IDD_CONFIGURE_LOCATION };

	protected:
		virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support

		DECLARE_MESSAGE_MAP()
	public:
		/** Initializing the dialog */
		virtual BOOL OnInitDialog();

		/** Setup the tool tips */
		void InitToolTips();

		/** Called when this page is no longer the active page */
		virtual BOOL OnKillActive();

		/** Handling the tool tips */
		virtual BOOL PreTranslateMessage(MSG* pMsg); 

		/** The user has changed the name of the volcano */
		afx_msg void OnChangeVolcano();

		/** The user has changed the model of the spectrometer */
		afx_msg void OnChangeModel();

		/** The user has changed the type of the instrument */
		afx_msg void OnChangeType();

		/** The user has changed the kind of electronics */
		afx_msg void OnChangeElectronics();

		/** The user has changed the number of channels in the spectrometer */
		afx_msg void OnChangeChannelNum();

		/** Saving the data in the dialog */
		afx_msg void SaveData();

		/** Updating the data in the dialog */
		afx_msg void UpdateDlg();

		/** Updating the name of the observatory in the combo-box */
		void UpdateObservatoryName();

		/** Called when the user has changed the currently selected scanner */
		void OnChangeScanner();

	private:

		// The labels on the screen.
		CStatic  m_labelVolcano;
		CStatic  m_labelSite;
		CStatic  m_labelObservatory;


		// The edit controls on the screen.
		CStatic m_editSite, m_labelSPR;
		CEdit   m_editSPR1, m_editSPR2, m_editMSC1, m_editMSC2;
		CStatic m_editSerial;

		// The combo controls on the screen
		CComboBox	m_comboVolcano;
		CComboBox	m_comboObservatory;
		CComboBox m_comboInstrumentType;
		CComboBox m_comboElectronics;
		CComboBox	m_comboSpectrometerModel;
		CComboBox	m_comboSpectrometerChannels;

		/** Adds a volcano to the list of volcanoes */
		void	AddAVolcano();

		/** (Re-)initializes the list of volcanoes */
		void UpdateVolcanoList();

	public:
		virtual BOOL OnSetActive();
	};
}