#include "StdAfx.h"
#include "pakfilehandler.h"
#include "SpectrumIO.h"
#include "../Common.h"

// the settings...
#include "../../Configuration/Configuration.h"

#include "ScanFileHandler.h"

using namespace FileHandler;

extern CFormView *pView;                 // <-- The screen
extern CConfigurationSetting g_settings; // <-- The settings

CPakFileHandler::CPakFileHandler(void)
{
	m_tempIndex = 0;
	m_initializedOutput = false;
	m_serialNumbers.SetSize(3); // make room for 3 spectrometers
}

CPakFileHandler::~CPakFileHandler(void)
{
	int N = m_serialNumbers.GetSize();
	for(int k = 0; k < N; ++k){
		CString *str = m_serialNumbers.GetAt(k);
		delete(str);
	}
	m_serialNumbers.RemoveAll();
}

void CPakFileHandler::InitializeDirectories(const CString &serialNumber, const CString *outputDir){
	CString message;
	if(!m_initializedOutput){
		if(outputDir == 0 || 0 != CreateDirectoryStructure(*outputDir)){
			m_tempDir.Format("%sTemp\\%s\\", g_settings.outputDirectory, serialNumber);
			m_incompleteDir.Format("%s\\IncompleteScans\\", m_tempDir);
			m_lostDir.Format("%sLost\\%s", g_settings.outputDirectory, serialNumber);
		}else{
			m_tempDir.Format("%s\\%s\\", *outputDir, serialNumber);
			m_incompleteDir.Format("%s\\IncompleteScans\\", m_tempDir);
			m_lostDir.Format("%s", m_incompleteDir);
		}

		if(CreateDirectoryStructure(m_tempDir)){        // create the temporary directory
			message.Format("Could not create temp-directory; %s", m_tempDir);
			ShowMessage(message);
		}
		if(CreateDirectoryStructure(m_lostDir)){				// create the lost directory
			message.Format("Could not create lost-directory; %s", m_lostDir);
			ShowMessage(message);
		}
		if(CreateDirectoryStructure(m_incompleteDir)){  // create the incomplete-scans directory
			message.Format("Could not create incomplete-directory; %s", m_incompleteDir);
			ShowMessage(message);
		}

		m_initializedOutput = true;
	}
}

int CPakFileHandler::FindFirstScanStart(const CString &fileName, const CString &fileForLost){
	int specNum = 0;
	CSpectrum spec;
	CString message;
	SpectrumIO::CSpectrumIO reader;
	SpectrumIO::CSpectrumIO writer;

	while(SUCCESS == reader.ReadSpectrum(fileName, specNum++, spec)){
		if(spec.ScanIndex() != 0){
			// Add the spectrum to the 'lost' file
			if(fileForLost.GetLength() > 3 || fileForLost.GetLength() < MAX_PATH){
				writer.AddSpectrumToFile(fileForLost, spec);
			}
		}else{
			// We found what we were looking for, jump out of the loop
			--specNum;
			break;
		}
	}

	return specNum;
}

/** Finds the next spectrum in the already opened file, which is the first
		spectrum of a scan. The ignored spectra are saved in the 'incomplete' folder.
		On succesful return the function will return SUCCESS and the provided
		spectrum 'curSpec' will be the next first spectrum of a scan. 
		If there is no more scan-start spectrum in the file, this function will
		return FAIL. */
RETURN_CODE CPakFileHandler::FindNextScanStart(FILE *pakFile, CSpectrum &curSpec){ 
	SpectrumIO::CSpectrumIO reader;
	SpectrumIO::CSpectrumIO writer;
	CString incompleteFileName, serialNumber, message;
	char *spectrumHeader = (char*)calloc(HEADER_BUF_SIZE, sizeof(char)); // <-- the spectrum header, in binary format
	int specHeaderSize = 0;
	int originalSpectrumNumber = m_spectrumNumber;

	// Get the serial-number of the spectrometer
	serialNumber.Format("%s", curSpec.m_info.m_device);

	// Get the spectrum's position in the scan
	int scanIndex = curSpec.ScanIndex(); 
	int originalScanIndex = scanIndex;

	// Save this spectrum in a file in the 'incomplete' - folder
	CSpectrumTime *starttid = &curSpec.m_info.m_startTime;
	incompleteFileName.Format("%s\\%s_%02d.%02d.%02d.pak", m_incompleteDir, serialNumber, starttid->hr, starttid->m, starttid->sec);

	// Continue reading spectra until we find one which has scan-index = 0
	while(scanIndex > 0){
		// write the spectrum to an incomplete-file and store it...
		writer.AddSpectrumToFile(incompleteFileName, curSpec, spectrumHeader, specHeaderSize);

		// Read the next spectrum from the file and see if this is the beginning
		//	of a scan...
		RETURN_CODE ret = reader.ReadNextSpectrum(pakFile, curSpec, specHeaderSize, spectrumHeader, HEADER_BUF_SIZE);
		if(ret == FAIL){
			if(SUCCESS != HandleCorruptSpectrum(reader, pakFile)){
				free(spectrumHeader);
				return FAIL;
			}
		}else{
			++m_spectrumNumber;
		}

		// Get the spectrums position in the scan
		scanIndex = curSpec.ScanIndex();
	}

	// Tell the user what just happened
	int nSkipped = m_spectrumNumber - originalSpectrumNumber;
	if(originalScanIndex > 1)
		message.Format("Recieved a scan where the first %d spectra were missing. ", originalScanIndex);
	else
		message.Format("Recieved a scan where the first spectrum was missing. ");
	if(nSkipped > 1)
		message.AppendFormat("%d spectra ignored and moved to the incomplete folder", nSkipped);
	else
		message.AppendFormat("1 spectrum ignored and moved to the incomplete folder");

	ShowMessage(message);

	free(spectrumHeader);

	return SUCCESS;
}

/** Sends a message to the evaluation thread that this scan-file should
		be evaluated. The file will first be moved to a temporary file
		so that nothing */
RETURN_CODE CPakFileHandler::EvaluateScan(const CString &fileName, const CString &serialNumber){

	return SUCCESS;
}

/** This function reads a downloaded pak-file and checks it for errors.
		If all is ok, the scans in the file are evaluated.
		@return 1 if anything in the file is wrong and it should be downloaded again.
		@return 0 if all is ok. */
int CPakFileHandler::ReadDownloadedFile(const CString &fileName, bool deletePakFile, bool evaluate, const CString *outputDir){
	CString message;
	SpectrumIO::CSpectrumIO reader;
	SpectrumIO::CSpectrumIO writer;
	reader.m_logFileWriter = NULL; // nowhere to output the error messages
	CSpectrum curSpec;
	CSpectrum *mSpec[MAX_CHANNEL_NUM]; // <-- An array of spectra, needed if an multichannel spectrum is coming in.
	CString lostFile[MAX_CHANNEL_NUM]; // <-- Where to move the lost spectra
	int numSpecRead[MAX_CHANNEL_NUM];  // <-- The number of read spectra in the last started scan
	int	nSpecPerScan[MAX_CHANNEL_NUM]; // <-- The number of spectra per scan in the last started scan
	CString serialNumber; // <-- The serial number of the spectrometer that generated the spectra
	int i, old_scanIndex[MAX_CHANNEL_NUM], repetitions[MAX_CHANNEL_NUM];
	int channelFrom, channelTo;
	unsigned char channel;
	int nEvaluatedScans = 0;
	bool isMultiChannelSpec = false;

	char *spectrumHeader = (char*)calloc(HEADER_BUF_SIZE, sizeof(char)); // <-- the spectrum header, in binary format
	int specHeaderSize = 0;

	// 0. Reset 
	m_spectrumNumber = 0;

	// 1. Test the file, make sure that it exists and that we can read it 
	if(!IsExistingFile(fileName)){
		ShowMessage("Error - cannot find the downloaded file.");
		free(spectrumHeader);
		return 1;
	}else{
		message.Format("Checking downloaded file %s for errors", fileName);
		ShowMessage(message);
	}

	// 2. Read the first spectrum in the file
	if(SUCCESS != reader.ReadSpectrum(fileName, 0, curSpec)){
		ShowMessage("Cannot read first spectrum from the downloaded file.");
		channel = 0; // assumption
		serialNumber.Format("NN");
	}else{
		channel = curSpec.Channel();
		if(channel < 0){
			// There's something wrong with this channel number
			ShowMessage("Received file with illegal channel number");
			channel = 0;
		}
		serialNumber.Format("%s", curSpec.m_info.m_device);
		if(serialNumber.GetLength() < 6 || serialNumber.GetLength() > 11){
			// There's something wrong with this serial-number
			serialNumber.Format("NN");
			ShowMessage("Received file with illegal serial number");
		}
	}

	// 3. Make sure that we have all the directories that we want to use
	InitializeDirectories(serialNumber, outputDir);

	// 4. Define the file-names that we will/might use
	for(i = 0; i < MAX_CHANNEL_NUM; ++i){
		int tmpInt = 0;
		m_scanFile[i].Format("%s\\Scan_%05d_%1d.pak", m_tempDir, tmpInt++, i); // the file containing the spectra from one channel
		while(IsExistingFile(m_scanFile[i])){
			m_scanFile[i].Format("%s\\Scan_%05d_%1d.pak", m_tempDir, tmpInt++, i); // the file containing the spectra from one channel
		}
		lostFile[i].Format("%s\\Incomplete_%s_%1d.pak", m_lostDir, serialNumber, i);
		numSpecRead[i]  = 0;
		nSpecPerScan[i] = 0;
		mSpec[i] = new CSpectrum();
	}
	
	// 5. Find the first spectrum in the file which is the first spectrum in a scan.
	//		The spectra before that will be thrown away to 'lostFile[channel]'
	if(channel < MAX_CHANNEL_NUM){
		m_spectrumNumber = FindFirstScanStart(fileName, lostFile[channel]);
	}else{
		m_spectrumNumber = FindFirstScanStart(fileName, lostFile[0]);
	}
	// 6. Read all the spectra in the newly recieved file and when 
	//		we've read a full scan, evaluate it.
	memset(old_scanIndex, -1, MAX_CHANNEL_NUM*sizeof(int));
	memset(repetitions, 0, MAX_CHANNEL_NUM*sizeof(int));
	FILE *pakFile = fopen(fileName, "rb");
	if(pakFile == NULL){
		message.Format("CPakFileHandler: Could not open .pak-file %s", fileName);
		ShowMessage(message);
	}else{
		while(1){
			RETURN_CODE ret = reader.ReadNextSpectrum(pakFile, curSpec, specHeaderSize, spectrumHeader, HEADER_BUF_SIZE);
			if(ret == FAIL){
				// If the spectrum is corrupt, save it to the 'corrupted' - folder
				if(reader.m_lastError == SpectrumIO::CSpectrumIO::ERROR_CHECKSUM_MISMATCH){
					SaveCorruptSpectrum(curSpec, specHeaderSize, spectrumHeader);
				}
				if(SUCCESS != HandleCorruptSpectrum(reader, pakFile))
					break;
			}else{
				++m_spectrumNumber;
			}

			// 6a. Get the channel the spectrum was collected with
			channel             = curSpec.Channel();
			isMultiChannelSpec  = CorrectChannelNumber(channel);

			if(channel >= MAX_CHANNEL_NUM){
				// This is not handled by the program
				message.Format("Recieved spectrum with channel %d. Program not able to handle more than %d channels. Please contact mattias.johansson@chalmers.se or bo.galle@chalmers.se about this problem", channel, MAX_CHANNEL_NUM);
				ShowMessage(message);
				continue;
			}

			// 6b. Get the scan index
			short scanIndex = curSpec.ScanIndex();

			// 6c. Get the range of channels that are contained in this spectrum.
			if(isMultiChannelSpec){
				channelFrom = 0;
				channelTo   = curSpec.Split(mSpec);
			}else{
				channelFrom = channel;
				channelTo   = channel + 1;
			}

			// 6d. Loop through all the channel-numbers that are stored in this spectrum.
			for(int k = channelFrom; k < channelTo; ++k){
	
				// Make sure that we don't go out of bounds here...
				if(k < 0 || k > MAX_CHANNEL_NUM){
					ShowMessage("Illegal channel number in CPakFileHandler::ReadDownloadedFile");
					continue;
				}

				// 6d1. If this spectrum is the beginning of a scan, then
				//			the files that we've read is a complete scan. 
				//			Evaluate it, move the scan file and start filling up a scan-file
				// 6d2. Also, if the scan-index of this spectrum is lower
				//			than the scan-index of the spectrum before, then we've probably
				//			started on a scan.
				if((numSpecRead[k] > 0 && scanIndex == 0) || scanIndex < old_scanIndex[k]){
					if(evaluate)
						EvaluateScan(m_scanFile[k], serialNumber);
					else
						ArchiveScan(m_scanFile[k]);
					numSpecRead[k] = 0;
					++nEvaluatedScans;

					// This should be the beginning of a scan, if not so then
					//	loop forwards until we find one sky-spectrum
					if(scanIndex > 0){
						if(SUCCESS != FindNextScanStart(pakFile, curSpec)) // <-- this changes the 'm_spectrumNumber' - variable
							break;
						else
							scanIndex = 0;
					}
				}

				// 6d3. Add the spectrum to the scan-file
				if(isMultiChannelSpec){
					writer.AddSpectrumToFile(m_scanFile[k], *mSpec[k]);
				}else{
					if(specHeaderSize > 0){
						writer.AddSpectrumToFile(m_scanFile[k], curSpec, spectrumHeader, specHeaderSize);
					}else{
						writer.AddSpectrumToFile(m_scanFile[k], curSpec);
					}
				}

				// 6d4. If this measurement represents a 'new' measurement-line
				//			then increase the number of spectra read.
				//			(Don't count repetitions on the same measurement - line)
				if(old_scanIndex[k] != scanIndex){
					++numSpecRead[k];
					old_scanIndex[k] = scanIndex;
				}else{
					++repetitions[k];
				}
			}
		}

		fclose(pakFile);
	}//endif

	// 7. If we've read equally many spectra as measurement-lines in the 
	//		cfg.txt-file, then assume that this is a full scan
	if(isMultiChannelSpec){
		// 7a. The last spectrum was a multichannel spectrum
		int nChannels = curSpec.Split(mSpec);
		if(nChannels <= MAX_CHANNEL_NUM){
			for(int k = 0; k < nChannels; ++k){
				if(numSpecRead[k] == mSpec[k]->SpectraPerScan()){
					if(evaluate)
						EvaluateScan(m_scanFile[k], serialNumber);
					else
						ArchiveScan(m_scanFile[k]);
					numSpecRead[k] = 0;
					++nEvaluatedScans;
				}
			}
		} // endif(nChannels...
	}else{
		// 7b. The last spectrum was a normal spectrum
		if(channel >= 0 && channel < MAX_CHANNEL_NUM){
			if(numSpecRead[channel] == curSpec.SpectraPerScan()){
				if(evaluate)
					EvaluateScan(m_scanFile[channel], serialNumber);
				else
					ArchiveScan(m_scanFile[channel]);
				numSpecRead[channel] = 0;
				++nEvaluatedScans;
			}
		}
	}

	// 8. If no scans were evaluated, tell the user...
	if(nEvaluatedScans == 0){
		ShowMessage("Downloaded file does not contain a complete scan.");
		for(i = 0; i < MAX_CHANNEL_NUM; ++i){
			if(nSpecPerScan[i] == 0)
				continue;
			message.Format("Channel %d: File contains %d spectra, expected %d.",
				i, numSpecRead[i], nSpecPerScan[i]);
			ShowMessage(message);
		}
	}

	// 9. We have read all the scans in the file. If the scan is not complete 
	//		save it in the 'incomplete' folder
	for(i = 0; i < MAX_CHANNEL_NUM; ++i){
		CString incompleteFileName;
		if(numSpecRead[i] != 0){
			// 9a. Find a good name for the incomplete-file
			incompleteFileName.Format("%s\\%s_%1d.pak", m_incompleteDir, serialNumber, i);
			
			// 9b. If there's already an incomplete-file with this filename
			//      then move it to the 'lost' folder.
			if(IsExistingFile(incompleteFileName)){
				int index = GetSpectrometerIndex(serialNumber);
				int it = 1 + m_lastLostIndex.GetAt(index);
				while(IsExistingFile(lostFile[i]))
					lostFile[i].Format("%s\\Incomplete_%s_%1d_%d.pak", m_lostDir, serialNumber, i, it++);

				m_lastLostIndex.SetAt(index, it);
				if(0 == MoveFile(incompleteFileName, lostFile[i])){
					// TODO!!!
					ShowMessage("Error, could not move incomplete file!!");
				}else{
					ShowMessage("Moved incomplete scan file to folder for lost scans");
					int volcanoIndex = Common::GetMonitoredVolcano(serialNumber);
					UploadToNOVACServer(lostFile[i], volcanoIndex);
				}
			}

			// 9c. Move the spectra to the incomplete-file
			if(0 == MoveFile(m_scanFile[i], incompleteFileName)){
				// TODO!!!
				ShowMessage("Error, could not move file");
			}

			// 9d. Tell The user what happened...
			ShowMessage("Recieved incomplete scan. Scan-file moved to folder for incomplete scans");
		}
	}// end for...
	
	// 10. Clean up a little bit...
	for(i = 0; i < MAX_CHANNEL_NUM; ++i)
		delete(mSpec[i]);
	free(spectrumHeader);

	// 11. Before returning, delete the file
	if(deletePakFile)
		if(0 == DeleteFile(fileName))
			return 1;

	return 0;
}

/** Looks up the index for the supplied serialNumber into the array of 
		serialNumbers. If the serialNumber does not exist in the array, it will be
		inserted and it's index will be returned */
int CPakFileHandler::GetSpectrometerIndex(const CString &serialNumber){
	CString message;
	int ret = 0;
	int nSpec = (int)m_serialNumbers.GetSize();

	// look for the serial number in the array
	for(ret = 0; ret < nSpec; ++ret){
		CString *str = m_serialNumbers.GetAt(ret);
		if(str == NULL || str->GetLength() == 0)
			break;

		if(Equals(serialNumber, *str))
			return ret;
	}

	// the serial number does not exist in the array, insert it!
	m_serialNumbers.SetAtGrow(ret, new CString(serialNumber));
	m_lastLostIndex.SetAtGrow(ret, 0);

	return ret;
}

/** This function checks the contents of the .pak-file 'fileName' 
		and returns the type of measurement which is inside the file */
MEASUREMENT_MODE CPakFileHandler::GetMeasurementMode(const CString &fileName){
	if(CPakFileHandler::IsStratosphericMeasurement(fileName)){
		return MODE_STRATOSPHERE;
	}else if(CPakFileHandler::IsDirectSunMeasurement(fileName)){
		return MODE_DIRECT_SUN;
	}else if(CPakFileHandler::IsLunarMeasurement(fileName)){
		return MODE_LUNAR;
	}else if(CPakFileHandler::IsWindSpeedMeasurement(fileName)){
		return MODE_WINDSPEED;
	}else if(CPakFileHandler::IsCompositionMeasurement(fileName)){
		return MODE_COMPOSITION;
	}else{
		// if nothing else then assume that this is a flux-measurement
		return MODE_FLUX;
	}
}

/** This function checks the contents of the file 'fileName'.
		@return true - if the spectra are collected in a wind speed measurement mode. 
		@return false - if the file does not contain spectra, 
				or contains spectra which are not collected in a wind speed measurement mode. */
bool CPakFileHandler::IsWindSpeedMeasurement(const CString &fileName){
	if(IsWindSpeedMeasurement_Gothenburg(fileName))
		return true;
	if(IsWindSpeedMeasurement_Heidelberg(fileName))
		return true;

	// not a wind-speed measurement file
	return false;
}
/** This function checks the contents of the file 'fileName'.
		@return true - if the spectra are collected in a wind speed measurement mode. 
		@return false - if the file does not contain spectra, 
				or contains spectra which are not collected in a wind speed measurement mode. */
bool CPakFileHandler::IsWindSpeedMeasurement_Gothenburg(const CString &fileName){
	SpectrumIO::CSpectrumIO reader;
	double scanAngle = 0, scanAngle2 = 0;
	CSpectrum spectrum;
	CDateTime gpsTime;
	double saz, sza;
	int nRepetitions = 0; // <-- The number of repetitions at one specific scan angle

	// 1. Count the number of spectra
	int numSpec = reader.CountSpectra(fileName);
	if(numSpec <= 2)
		return false; // <-- If file is not readable/empty/contains only a few spectra then return false.

	// 2. Check the Solar Zenith Angle at the time when the measurement started
	//			If this is larger than 75, then the measurement is a stratospheric measurement
	//			and not a wind-speed measurement
	if(SUCCESS != reader.ReadSpectrum(fileName, 0, spectrum))
		return false;
	gpsTime.year   = spectrum.m_info.m_date[0];
	gpsTime.month  = (unsigned char)spectrum.m_info.m_date[1];
	gpsTime.day    = (unsigned char)spectrum.m_info.m_date[2];
	gpsTime.hour   = (unsigned char)spectrum.m_info.m_startTime.hr;
	gpsTime.minute = (unsigned char)spectrum.m_info.m_startTime.m;
	gpsTime.second = (unsigned char)spectrum.m_info.m_startTime.sec;
	if(SUCCESS != Common::GetSunPosition(gpsTime, spectrum.Latitude(), spectrum.Longitude(), sza, saz))
		return false;
	if(fabs(sza) > 75)
		return false;

	/* In a windspeed measurement we expect to have one/several sky+dark spectra
		and then a long series of measurements at one single scan angle. 

		- For a Gothenburg-type of instrument, a measurement is considered
			to be a wind-speed measurement if there are more than 50
			repetitions at a single scan-angle. 
		- For a Heidelberg-type of instrument, a measurement is considered
			to be a wind-speed measurement if there are more than 50 
			repetitions of two scan-angles */

	// 3. Go through the file, starting at the last spectrum in the file.
	if(SUCCESS != reader.ReadSpectrum(fileName, numSpec-3, spectrum))
		return false;
	scanAngle  = spectrum.ScanAngle();
	scanAngle2 = spectrum.ScanAngle2();

	for(int specIndex = numSpec-4; specIndex > 0; --specIndex){
		if(SUCCESS != reader.ReadSpectrum(fileName, specIndex, spectrum)){
			// failed to read the spectrum
			break;
		}
		// if this is the same scan angle as in the last spectrum, 
		//	then increase the number of repetitions.
		if((fabs(scanAngle - spectrum.ScanAngle()) < 1e-2) && (fabs(scanAngle2 - spectrum.ScanAngle2()) < 1e-2)){
			++nRepetitions;
		}else{
			break;
		}

		if(nRepetitions >= 50)
			return true;
	}

	// IF THERE ARE MORE THAN 50 REPETITIONS IN ONE SINGLE SCAN ANGLE
	//	THEN WE CONSIDER THIS MEASUREMENT TO BE A WINDSPEED MEASUREMENT
	if(nRepetitions >= 50)
		return true;

	return false;
}
/** This function checks the contents of the file 'fileName'.
	@return true - if the spectra are collected in a wind speed measurement mode. 
	@return false - if the file does not contain spectra, 
			or contains spectra which are not collected in a wind speed measurement mode. */
bool CPakFileHandler::IsWindSpeedMeasurement_Heidelberg(const CString &fileName){
	SpectrumIO::CSpectrumIO reader;
	double scanAngles[2]	= {0, 0};
	double scanAngles2[2]	= {0, 0};
	int		 scanIndex			= 0;
	CSpectrum spectrum;
	int nRepetitions = 0; // <-- The number of repetitions at one specific scan angle

	// 1. Count the number of spectra
	int numSpec = reader.CountSpectra(fileName);
	if(numSpec <= 2)
		return false; // <-- If file is not readable/empty/contains only a few spectra then return false.

	/* In a windspeed measurement we expect to have one/several sky+dark spectra
		and then a long series of measurements at one single scan angle. 

		- For a Gothenburg-type of instrument, a measurement is considered
			to be a wind-speed measurement if there are more than 50
			repetitions at a single scan-angle. NOTE! this must be separated
			from stratospheric measurements...
		- For a Heidelberg-type of instrument, a measurement is considered
			to be a wind-speed measurement if there are more than 50 
			repetitions of two scan-angles */

	// 2. Go through the file, starting at the last spectrum in the file.
	if(SUCCESS != reader.ReadSpectrum(fileName, numSpec-3, spectrum))
		return false;
	scanAngles[0]				= spectrum.ScanAngle();
	scanAngles2[0]			= spectrum.ScanAngle2();

	if(SUCCESS != reader.ReadSpectrum(fileName, numSpec-4, spectrum))
		return false;
	scanAngles[1]				= spectrum.ScanAngle();
	scanAngles2[1]			= spectrum.ScanAngle2();

	for(int specIndex = numSpec-5; specIndex > 0; --specIndex){
		if(SUCCESS != reader.ReadSpectrum(fileName, specIndex, spectrum)){
			// failed to read the spectrum
			break;
		}
		// if this is the same scan angle as in the last spectrum, 
		//	then increase the number of repetitions.
		if(fabs(scanAngles[scanIndex] - spectrum.ScanAngle()) < 1e-2){
			if(fabs(scanAngles2[scanIndex] - spectrum.ScanAngle2()) < 1e-2){
				++nRepetitions;
				scanIndex	= (scanIndex + 1) % 2;
			}
		}else{
			break;
		}

		if(nRepetitions >= 50)
			return true;
	}

	// IF THERE ARE MORE THAN 50 REPETITIONS IN ONE SINGLE SCAN ANGLE
	//	THEN WE CONSIDER THIS MEASUREMENT TO BE A WINDSPEED MEASUREMENT
	if(nRepetitions >= 50)
		return true;

	return false;
}

/** This function checks the contents of the file 'fileName'.
		@return true - if the spectra are collected in a stratospheric measurement mode. 
		@return false - if the file does not contain spectra, 
				or contains spectra which are not collected in a stratospheric measurement mode. */
bool CPakFileHandler::IsStratosphericMeasurement(const CString &fileName){
	SpectrumIO::CSpectrumIO reader;
	double scanAngle = 0;
	CSpectrum spectrum;
	CDateTime gpsTime;
	double saz, sza;
	int nRepetitions = 0; // <-- The number of repetitions at one specific scan angle

	// 1. Count the number of spectra
	int numSpec = reader.CountSpectra(fileName);
	if(numSpec <= 2 || numSpec > 50)
		return false; // <-- If file is not readable/empty/contains only a few spectra then return false.

	// 2. Check the Solar Zenith Angle at the time when the measurement started
	//			If this is larger than 75, then the measurement is a stratospheric measurement
	//			and not a wind-speed measurement
	if(SUCCESS != reader.ReadSpectrum(fileName, 0, spectrum))
		return false;
	gpsTime.year		= spectrum.m_info.m_date[0];
	gpsTime.month		= (unsigned char)spectrum.m_info.m_date[1];
	gpsTime.day			= (unsigned char)spectrum.m_info.m_date[2];
	gpsTime.hour		= (unsigned char)spectrum.m_info.m_startTime.hr;
	gpsTime.minute	= (unsigned char)spectrum.m_info.m_startTime.m;
	gpsTime.second	= (unsigned char)spectrum.m_info.m_startTime.sec;
	if(SUCCESS != Common::GetSunPosition(gpsTime, spectrum.Latitude(), spectrum.Longitude(), sza, saz))
		return false;
	if(fabs(sza) < 75)
		return false;

	// 3. Go through the file, starting at the second last spectrum in the file.
	for(int specIndex = numSpec-2; specIndex > 0; --specIndex){
		if(SUCCESS != reader.ReadSpectrum(fileName, specIndex, spectrum)){
			// failed to read the spectrum
			break;
		}
		// if this is the same scan angle as in the last spectrum, 
		//	then increase the number of repetitions.
		if(fabs(scanAngle) < 1e-2){
			++nRepetitions;
		}else{
			break;
		}

		/** If there are more than 3 repetitions in the zenith
				then the measurement is considered to be a stratospheric measurement */
		if(nRepetitions > 3)
			return true;
	}

	return false;
}

/** This function checks the contents of the file 'fileName'.
		@return true - if the spectra are collected in a direct-sun mode. 
		@return false - if the file does not contain spectra, 
				or contains spectra which are not collected in a direct-sun measurement mode. */
bool CPakFileHandler::IsDirectSunMeasurement(const CString &fileName){
	CScanFileHandler scan;
	CSpectrum spec;
	int nFound = 0;

	// It is here assumed that the measurement is a direct-sun measurment
	//	if there is at least 5 spectrum with the name 'direct_sun'
	if(SUCCESS != scan.CheckScanFile(&fileName)){
		return false; // failed to check the file
	}
	while(scan.GetNextSpectrum(spec)){
		CString name = CString(spec.m_info.m_name);
		if(Equals(name, "direct_sun")){
			++nFound;
			if(nFound == 5)
				return true;
		}
	}
	
	return false;
}
/** This function checks the contents of the file 'fileName'.
		@return true - if the spectra are collected in a lunar mode. 
		@return false - if the file does not contain spectra, 
				or contains spectra which are not collected in a lunar measurement mode. */
bool CPakFileHandler::IsLunarMeasurement(const CString &fileName){
	CScanFileHandler scan;
	CSpectrum spec;
	int nFound = 0;

	// It is here assumed that the measurement is a direct-sun measurment
	//	if there is at least 5 spectrum with the name 'direct_sun'
	if(SUCCESS != scan.CheckScanFile(&fileName)){
		return false; // failed to check the file
	}
	while(scan.GetNextSpectrum(spec)){
		CString name = CString(spec.m_info.m_name);
		if(Equals(name, "lunar")){
			++nFound;
			if(nFound == 5)
				return true;
		}
	}
	
	return false;
}

/** This function checks the contents of the file 'fileName'.
		@return true - if the spectra are collected in a calibration measurment - mode.
		@return false - if the file does not contain readable spectra,
				or contains spectra which are not collected in a calibration measurment - mode.*/
bool CPakFileHandler::IsCalibrationMeasurement(const CString &fileName){

	return false;
}

/** This function checks the contents of the file 'fileName'.
		@return true - if the spectra are collected in a composition measurment - mode.
		@return false - if the file does not contain readable spectra,
				or contains spectra which are not collected in a composition measurment - mode.*/
bool CPakFileHandler::IsCompositionMeasurement(const CString &fileName){
	CScanFileHandler scan;
	CSpectrum spec;

	// It is here assumed that the measurement is a composition measurment
	//	if there is 
	//		* at least 1 spectrum with the name 'offset'
	//		* at least 1 spectrum with the name 'dark_cur'
	//		* at least 1 spectrum with the name 'comp'
	if(SUCCESS != scan.CheckScanFile(&fileName)){
		return false; // failed to check the file
	}
	scan.ResetCounter();
	while(scan.GetNextSpectrum(spec)){
		CString name = CString(spec.m_info.m_name);
		if(Equals(name, "comp")){
			return true;
		}
	}

	return false;
}

/** Takes a scan file and renames it to an approprate name */
RETURN_CODE	CPakFileHandler::ArchiveScan(const CString &scanFileName){
	SpectrumIO::CSpectrumIO reader;
	reader.m_logFileWriter = NULL;	// nowhere to output the error messages

	CSpectrum tmpSpec;
	CString serialNumber, dateStr, timeStr, nowStr, pakFile;

	// 1. Read one spectrum in the scan
	int specIndex = 0;
	while(SUCCESS != reader.ReadSpectrum(scanFileName, specIndex++, tmpSpec)){
		if(reader.m_lastError == SpectrumIO::CSpectrumIO::ERROR_SPECTRUM_NOT_FOUND)
			return FAIL;
	}
	CSpectrumInfo &info	= tmpSpec.m_info;
	int channel         = info.m_channel;

	// 2. Get the serialNumber of the spectrometer
	serialNumber.Format("%s", info.m_device);

	// 3. Get the time and date when the scan started
	dateStr.Format("%02d%02d%02d", info.m_date[0] % 1000, info.m_date[1], info.m_date[2]);
	timeStr.Format("%02d%02d", info.m_startTime.hr, info.m_startTime.m);

	// 5. Write the archiving name of the spectrum file
	if(channel < 128 && channel > MAX_CHANNEL_NUM)
		channel = channel % 16;
	pakFile.Format("%s%s_%s_%s_%1d.pak", m_tempDir, serialNumber, dateStr, timeStr, channel);

	if(strlen(pakFile) > MAX_PATH)
		return FAIL;

	MoveFile(scanFileName, pakFile);

	return SUCCESS;
}

/** Adjusts the channel number to be in the range 0 - MAX_CHANNEL_NUM 
		@return true if the spectrum is a multichannel spectrum and should be split. */
bool CPakFileHandler::CorrectChannelNumber(unsigned char &channel){

	// If the channel number is >= 129 then the spectra are interlaced
	if(channel > 128){
		channel -= 128;
		return true;
	}

	// If the channel number in the spectral header is 128, then the user has 
	//	typed chn=256 in the cfg-file. This produces spectra only from the 
	//	master channel,even if the user might think otherwise
	if(channel == 128){
		channel = 0;
		return false;
	}

	// If the spectra are larger than 16, then they are single, interlaced spectra
	//	Get the channel number out from it...
	if(channel >= 16){
		channel = channel % 16;
		return false;
	}

	// unknown, should never get here
	return false;
}

/** Takes appropriate action when a corrupt spectrum has been found */
RETURN_CODE CPakFileHandler::HandleCorruptSpectrum(SpectrumIO::CSpectrumIO &reader, FILE *pakFile){
	CString str;
	if(reader.m_lastError == SpectrumIO::CSpectrumIO::ERROR_EOF || reader.m_lastError == SpectrumIO::CSpectrumIO::ERROR_COULD_NOT_OPEN_FILE || reader.m_lastError == SpectrumIO::CSpectrumIO::ERROR_SPECTRUM_NOT_FOUND)
		return FAIL;
	switch(reader.m_lastError){
		case SpectrumIO::CSpectrumIO::ERROR_CHECKSUM_MISMATCH:
			str.Format("Spectrum %d in pak file is corrupt, checksum mismatch", m_spectrumNumber);
			ShowMessage(str); break;
		case SpectrumIO::CSpectrumIO::ERROR_DECOMPRESS:
			str.Format("Spectrum %d in pak file is corrupt, spectrum could not be decompressed", m_spectrumNumber);
			ShowMessage(str); break;
	}
	//else
	++m_spectrumNumber;
	if(SUCCESS != reader.FindSpectrumNumber(pakFile, m_spectrumNumber))
		return FAIL;

	return SUCCESS;
}

/** Saves a newly found corrupted spectrum into the appropriate folder */
RETURN_CODE CPakFileHandler::SaveCorruptSpectrum(const CSpectrum &curSpec, int specHeaderSize, const char *spectrumHeader){
	CString directory, fileName, serial, date, time;

	ShowMessage("Corrupted spectrum found. Saving it in directory for corrupted spectra");

	// Crate the directory
	directory.Format("%s\\Corrupted\\", m_lostDir);
	if(CreateDirectoryStructure(directory)){
		ShowMessage("Could not create directory for corrupted spectra");
		return FAIL;
	}

	// Find a suitable file-name for the spectrum
	int index = 0;
	serial.Format(curSpec.m_info.m_device);
	date.Format("%04d.%02d.%02d", curSpec.m_info.m_date[0], curSpec.m_info.m_date[1], curSpec.m_info.m_date[2]);
	time.Format("%02d.%02d.%02d", curSpec.m_info.m_startTime.hr, curSpec.m_info.m_startTime.m, curSpec.m_info.m_startTime.sec);
	fileName.Format("%s%s_%s_%s_%d.pak", directory, serial, date, time, index++);
	while(IsExistingFile(fileName)){
		fileName.Format("%s%s_%s_%s_%d.pak", directory, serial, date, time, index++);
	}

	// Write the spectrum to a file
	// TODO: THIS IS TEMPORARILY COMMENTED OUT IN SEARCH FOR THE CRASH OF THE PROGRAM!!!
	// --------- KEEP THIS LINE COMMENTED OUT, IT SEEMS TO SOLVE THE CRASH PROBLEM -------
	//SpectrumIO::CSpectrumIO writer;
	//writer.AddSpectrumToFile(fileName, curSpec, spectrumHeader, specHeaderSize);

	return SUCCESS;
}
