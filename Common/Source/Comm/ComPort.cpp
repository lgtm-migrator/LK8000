/*
 * LK8000 Tactical Flight Computer -  WWW.LK8000.IT
 * Released under GNU/GPL License v.2 or later
 * See CREDITS.TXT file for authors and copyrights
 *
 * File:   ComPort.cpp
 * Author: Bruno de Lacheisserie
 *
 * Created on 28 juillet 2013, 16:15
 */
#include "externs.h"
#include <stdarg.h>
#include "ComPort.h"
#include <iterator>
#include <algorithm>
#include "Dialogs.h"
#include <functional>
#include <stdarg.h>
#include <stdio.h>
#include "Poco/RunnableAdapter.h"
#include "ComCheck.h"
#include <sstream>

ComPort::ComPort(int idx, const tstring& sName) : StopEvt(false), devIdx(idx), sPortName(sName) {
    pLastNmea = std::begin(_NmeaString);
}

ComPort::~ComPort() {
    assert(!ReadThread.isRunning());
}

bool ComPort::Close() {
    StopRxThread();
    return true;
}

namespace {

    tstring thread_name() {
        Poco::Thread* pThread = Poco::Thread::current();
        if (pThread) {
            return to_tstring(pThread->getName());
        }
        std::ostringstream name;
    	name << '#' << Poco::Thread::currentTid();
	    return to_tstring(name.str());
    }

    tstring data_string(const void *data, size_t size) {
        return to_tstring(std::string(static_cast<const char*>(data), size));
    }

}

// this is used by all functions to send data out
bool ComPort::Write(const void *data, size_t size) {
    bool success = Write_Impl(data, size);

    DebugLog(_T(R"(<%s><%s> ComPort::Write("%s"))"),
                success ? _T("success"): _T("failed"),
                thread_name().c_str(),
                data_string(data, size).c_str());

    return success;
}

#ifdef  _UNICODE
// this should be deprecated
void ComPort::WriteString(const wchar_t* Text) {
    const std::string data = to_utf8(Text);
    if (!data.empty()) {
        // don't write trailing '\0' to device
        Write(data.data(), data.size());
    }
}
#endif

void ComPort::WriteString(const char* Text) {
    const size_t length = strlen(Text);
    if(length > 0) {
        Write(Text, length);
    }
}

int ComPort::GetChar() {
    char c = EOF;
    if (Read(&c, 1) == 1) {
        return c;
    }
    return EOF;
}

bool ComPort::StopRxThread() {

    StopEvt.set();

    DebugLog(_T("... ComPort %u StopRxThread: Cancel Wait Event !"), (unsigned)(GetPortIndex() + 1));
    CancelWaitEvent();

    if (ReadThread.isRunning()) {
        DebugLog(_T("... ComPort %u StopRxThread: Wait End of thread !"), (unsigned)(GetPortIndex() + 1));
        ReadThread.join();
    }
    StopEvt.reset();

    return true;
}

bool ComPort::StartRxThread() {
  try {
      StopEvt.reset();

      std::ostringstream ss;
      ss << "ComPort " << 'A' + GetPortIndex();
      ReadThread.setName(ss.str());

      // Create a read thread for reading data from the communication port.
      ReadThread.start(*this);
      ReadThread.setPriority(Poco::Thread::PRIO_NORMAL); //THREAD_PRIORITY_ABOVE_NORMAL

      if (!ReadThread.isRunning()) {
          // Could not create the read thread.
          StartupStore(_T(". ComPort %u <%s> Failed to start Rx Thread%s"),
                       (unsigned) (GetPortIndex() + 1), GetPortName(), NEWLINE);

          // LKTOKEN  _@M761_ = "Unable to Start RX Thread on Port"
          StatusMessage(mbOk, MsgToken(266), TEXT("%s %s"), MsgToken(761), GetPortName());
          //DWORD dwError = GetLastError();
          return false;
      }
      return true;
  } catch(Poco::Exception& e) {
      const tstring error = to_tstring(e.displayText());
      StartupStore(_T(". ComPort %u <%s> StartRxThread : %s"), (unsigned)(GetPortIndex() + 1), GetPortName(), error.c_str());
      return false;
  } catch(std::exception& e) {
      const tstring error = to_tstring(e.what());
      StartupStore(_T(". ComPort %u <%s> StartRxThread : %s"), (unsigned)(GetPortIndex() + 1), GetPortName(), error.c_str());
      return false;
  }
}

void ComPort::run() {

    StartupStore(_T(". ComPort %u ReadThread : started"), (unsigned)(GetPortIndex() + 1));

    RxThread();
    SetPortStatus(CPS_OPENKO);

    StartupStore(_T(". ComPort %u ReadThread : terminated"), (unsigned)(GetPortIndex() + 1));
}

void ComPort::ProcessChar(char c) {
    if (ComCheck_ActivePort>=0 && GetPortIndex()==(unsigned)ComCheck_ActivePort) {
        ComCheck_AddChar(c);
    }

    if (devParseStream(devIdx, &c, 1, &GPS_INFO)) {
        // if this port is used for stream device, leave immediately.
        // don't return mayby more devices on one Port (shared Port)
    }

    // last char need to be reserved for '\0' for avoid buffer overflow
    // in theory this should never happen because NMEA sentence can't have more than 82 char and _NmeaString size is 160.
    if (pLastNmea >= std::begin(_NmeaString) && (pLastNmea+1) < std::end(_NmeaString)) {

        if (c == '\n' || c == '\r') {
            // abcd\n , now closing the line also with \r
            *(pLastNmea++) = _T('\n');
            *(pLastNmea) = _T('\0'); // terminate string.
            // process only meaningful sentences, avoid processing a single \n \r etc.
            if (std::distance(std::begin(_NmeaString), pLastNmea) > 5) {
                devParseNMEA(devIdx, _NmeaString, &GPS_INFO);
            }
        } else {
            *(pLastNmea++) = c;
            return;
        }
    }
    // overflow, so reset buffer
    pLastNmea = std::begin(_NmeaString);
}

void ComPort::AddStatRx(unsigned dwBytes) {
    if (GetPortIndex() < NUMDEV) {
        DeviceList[GetPortIndex()].Rx += dwBytes;
    }
}

void ComPort::AddStatErrRx(unsigned dwBytes) {
    if (GetPortIndex() < NUMDEV) {
        DeviceList[GetPortIndex()].ErrRx += dwBytes;
    }
}

void ComPort::AddStatTx(unsigned dwBytes) {
    if (GetPortIndex() < NUMDEV) {
        DeviceList[GetPortIndex()].Tx += dwBytes;
    }
}

void ComPort::AddStatErrTx(unsigned dwBytes) {
    if (GetPortIndex() < NUMDEV) {
        DeviceList[GetPortIndex()].ErrTx += dwBytes;
    }
}

void ComPort::SetPortStatus(int nStatus) {
    if (GetPortIndex() < NUMDEV) {
        DeviceList[GetPortIndex()].Status = nStatus;
    }
}

void ComPort::StatusMessage(MsgType_t type, const TCHAR *caption, const TCHAR *fmt, ...) {
    TCHAR tmp[127];
    va_list ap;
    LKASSERT(fmt!=NULL);

    va_start(ap, fmt);
    gcc_unused int n = _vsntprintf(tmp, 127, fmt, ap);
    va_end(ap);
    LKASSERT(n>=0); // Message to long for "tmp" buffer


    tmp[126] = _T('\0');

    if (caption) {
        MessageBoxX(tmp, LKGetText(caption), type);
    } else {
        DoStatusMessage(tmp);
    }
}
