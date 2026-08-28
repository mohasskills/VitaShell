#ifndef __LOCALSEND_DIALOG_H__
#define __LOCALSEND_DIALOG_H__

enum LocalSendDialogStatus {
  LOCALSEND_DIALOG_CLOSED,
  LOCALSEND_DIALOG_CLOSING,
  LOCALSEND_DIALOG_OPENED,
  LOCALSEND_DIALOG_OPENING,
};

int getLocalSendDialogStatus();
int initLocalSendDialog();
void localsendDialogCtrl();
void drawLocalSendDialog();

#endif
