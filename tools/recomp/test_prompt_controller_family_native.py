"""Compile the production family selector; keyboard must retain the pad family."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
text=(root/'src/input/xinput_device.c').read_text()
text=text[text.index('static PromptPadActivity'):text.index('static XBOX_INPUT_MAPPER')]
source='''#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include "xinput_xbox.h"
#include "prompt_activity.h"
'''+text+'''
int main(void){
 XBOX_GAMEPAD pad={0};
 assert(xbox_InputPromptController()==XBOX_PROMPT_XBOX);
 input_prompt_pad(XBOX_PROMPT_PLAYSTATION,&pad);
 assert(xbox_InputPromptController()==XBOX_PROMPT_PLAYSTATION);
 xbox_InputNotifyKeyboardActivity();
 assert(xbox_InputPromptDevice()==XBOX_PROMPT_KEYBOARD);
 assert(xbox_InputPromptController()==XBOX_PROMPT_PLAYSTATION);
 input_prompt_pad(XBOX_PROMPT_XBOX,&pad); /* idle second pad cannot steal family */
 assert(xbox_InputPromptController()==XBOX_PROMPT_PLAYSTATION);
 pad.bAnalogButtons[0]=255;input_prompt_pad(XBOX_PROMPT_XBOX,&pad);
 assert(xbox_InputPromptController()==XBOX_PROMPT_XBOX);
 xbox_InputNotifyKeyboardActivity();input_prompt_pad(XBOX_PROMPT_XBOX,&pad);
 assert(xbox_InputPromptDevice()==XBOX_PROMPT_KEYBOARD);
 pad.bAnalogButtons[1]=255;input_prompt_pad(XBOX_PROMPT_PLAYSTATION,&pad);
 assert(xbox_InputPromptDevice()==XBOX_PROMPT_PLAYSTATION);
 xbox_InputNotifyKeyboardActivity();
 assert(xbox_InputPromptController()==XBOX_PROMPT_PLAYSTATION);
 puts("controller family: connected pad, keyboard retention, idle/held input and active family switching pass");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-pad-family-') as td:
 p=Path(td);(p/'test.c').write_text(source)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-I'+str(root/'src/input'),'-I'+str(root/'include'),'-I'+str(root/'src'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
