import wx, re
from wxPython import wizard as wxWizard
from xmlwrapper import XMLWrapper
from xml.dom import minidom
from common import *

DOS_DEVICES = '\\DosDevices\\'

class BlockDevicesOptionArray(OptionArrayPanel):
    CLASS_DESC = 'Storage Devices'
    SHORT_TREE_DESC = 'Storage'
    LONG_DESC = """Storage devices are represented in Linux as /dev/cobdX, where
X is the number identifying the storage device."""

    class Item(ConfigurationItem):
        def title(self, long=False):
            index = int(self._xml_item.attr.index)
            if long:
                name = '/dev/cobd%d' % (index, )
            else:
                name = '[%d]' % (index, )
            path = self._xml_item.attr.path
            if path.startswith(DOS_DEVICES):
                title = str(path[len(DOS_DEVICES):])
            else:
                title = path
            title = '%s: %s' % (name, title)
            return title

    class ItemPanel(OptionArrayPanel.ItemPanel):
        def populate(self, panel_sizer):
            sizer = wx.BoxSizer(wx.VERTICAL)

            sizer2 = wx.BoxSizer(wx.HORIZONTAL)
            text = wx.StaticText(self.panel, -1, 'Filename: ')
            sizer2.Add(text, 0, 0, 3)
            self.path_text = textctrl = wx.TextCtrl(self.panel, -1, self.set_path())
            wx.EVT_TEXT(textctrl, -1, self.changed)
            sizer2.Add(textctrl, 1, wx.EXPAND, 3)
            button = wx.Button(self.panel, -1, "Browse")
            wx.EVT_BUTTON(button, -1, self.browse)
            sizer2.Add(button, 0, 0, 3)
            sizer.Add(sizer2, 0, wx.EXPAND | wx.ALL, 3)

            sizer2 = wx.BoxSizer(wx.HORIZONTAL)
            text = wx.StaticText(self.panel, -1, 'Index: ')
            sizer2.Add(text, 0, 0, 3)
            self.index_text = textctrl = wx.TextCtrl(self.panel, -1, self.item._xml_item.attr.index)
            wx.EVT_TEXT(textctrl, -1, self.changed)
            sizer2.Add(textctrl, 0, 0, 3)
            sizer.Add(sizer2, 0, wx.EXPAND | wx.ALL, 3)

            sizer2 = wx.BoxSizer(wx.HORIZONTAL)
            self.enabled = checkctrl = wx.CheckBox(self.panel, -1, "Enabled")
            sizer2.Add(checkctrl, 0, 0, 3)
            checkctrl.SetValue(self.item._xml_item.attr.enabled == 'true')
            wx.EVT_CHECKBOX(checkctrl, -1, self.changed)
            sizer.Add(sizer2, 0, wx.EXPAND | wx.ALL, 3)

            panel_sizer.Add(sizer, 0, wx.EXPAND | wx.ALL)

        def browse(self, event):
            dlg = wx.FileDialog(self.panel, 'Open', '', '', '*', wx.OPEN | wx.CHANGE_DIR)
            if dlg.ShowModal() == wx.ID_OK:
                path = dlg.GetPath()
                dlg.Destroy()
                self.pathtext.SetValue(path)

        def set_path(self):
            path = self.item._xml_item.attr.path
            if path.startswith(DOS_DEVICES):
                m = re.match(r"([a-zA-Z]:.*)", path[len(DOS_DEVICES):])
                if m:
                    path = m.groups()[0]
            return path

        def get_path(self):
            path = self.path_text.GetValue()
            m = re.match(r"[a-zA-Z]:[\/].*", path)
            if m:
                path = DOS_DEVICES + path
            return path

        def apply(self, event=None):
            try:
                ret_index = self.check_index(self.index_text.GetValue(), self.item._xml_item.attr.index)
            except InvalidArrayIndexChosen, e:
                wx.MessageDialog(self.panel, e.args[0], style=wx.OK).ShowModal()
                return False

            self.item._xml_item.attr.path = self.get_path()
            self.item._xml_item.attr.index = str(int(self.index_text.GetValue()))
            self.item._xml_item.attr.enabled = ['false', 'true'][self.enabled.GetValue()]
            super(BlockDevicesOptionArray.ItemPanel, self).apply(event)

    def get_item_list(self):
        return self.mainframe.block_devices

    def add_wizard(self, mainframe):
        from blockdevice import run_wizard
        return run_wizard(mainframe)

def run_wizard(mainframe):
    data = minidom.Element("block_device")

    class PrevLinkedPage(wxWizard.wxPyWizardPage):
        def __init__(self, prev, wizard, *arg, **kw):
            wxWizard.wxPyWizardPage.__init__(self, wizard, *arg, **kw)
            self.wizard = wizard
            self.prev = prev
            self.populate()

        def populate(self):
            pass

        def GetPrev(self):
            return self.prev

    class RegularExistingFile(PrevLinkedPage):
        def populate(self):
            sizer = wx.BoxSizer(wx.VERTICAL)
            text = wx.StaticText(self, -1, 'Filename: ')
            sizer.Add(text, 0, wx.EXPAND | wx.ALL, 3)
            self.text = textctrl = wx.TextCtrl(self, -1, '')
            sizer.Add(textctrl, 0, wx.EXPAND | wx.ALL, 3)
            button = wx.Button(self, -1, "Browse")
            wx.EVT_BUTTON(button, -1, self.browse)
            sizer.Add(button, 0, wx.EXPAND | wx.ALL, 3)
            self.SetSizer(sizer)

        def finish(self):
            data.pathname = self.text.GetValue()

        def browse(self, event):
            dlg = wx.FileDialog(self, 'Open', '',
                                '', '*', wx.OPEN | wx.CHANGE_DIR)
            if dlg.ShowModal() == wx.ID_OK:
                path = dlg.GetPath()
                self.text.SetValue(path)
                dlg.Destroy()

        def GetNext(self):
            data.setAttribute('path', self.text.GetValue())
            data.setAttribute('enabled', 'true')

    class NewSparseFile(PrevLinkedPage):
        pass

    class RegularFile(PrevLinkedPage):
        def populate(self):
            sizer = wx.BoxSizer(wx.VERTICAL)
            choices = ["Use an existing file",
                       "Create a new sparse file", ]
            radio = wx.RadioBox(self, -1, "File type",
                                 choices=choices, style = wx.RA_SPECIFY_COLS, majorDimension=1)
            sizer.Add(radio, 0, wx.EXPAND | wx.ALL, 3)
            self.SetSizer(sizer)
            self.radio = radio

        def GetNext(self):
            next_classes = [
                RegularExistingFile,
                NewSparseFile,
            ]
            return next_classes[self.radio.GetSelection()](self, self.wizard)

    class ExistingDevice(wxWizard.wxPyWizardPage):
        def __init__(self, prev, wizard, *arg, **kw):
            wxWizard.wxPyWizardPage.__init__(self, wizard, *arg, **kw)
            self.prev = prev

        def GetPrev(self):
            return self.prev

    class FirstPage(wxWizard.wxPyWizardPage):
        def __init__(self, wizard, *arg, **kw):
            wxWizard.wxPyWizardPage.__init__(self, wizard, *arg, **kw)

            sizer = wx.BoxSizer(wx.VERTICAL)
            text = wx.StaticText(self, -1,
                                 "Please specify which kind of virtual disk you would like to add.")

            sizer.Add(text, 0, wx.EXPAND | wx.ALL, 1)
            choices = ["Raw file image",
                       "Use an existing partition or device", ]

            radio = wx.RadioBox(self, -1, "Virtual disk type",
                                 choices=choices, style = wx.RA_SPECIFY_COLS, majorDimension=1)

            text = wx.StaticText(self, -1, '\n'.join([
                "If you have a ready root file system image, you can use it",
                "by choosing '%s'." % (choices[0], ),
                "",
                "Otherwise, if you would like to use an already install Linux",
                "disk parition or storage device, choose '%s'." % (choices[1], ),
                '',
                ]))

            sizer.Add(text, 0, wx.EXPAND | wx.ALL, 1)
            sizer.Add(radio, 0, wx.EXPAND | wx.ALL, 1)
            sizer_center = wx.BoxSizer(wx.VERTICAL)
            sizer_center.Add(sizer, 0, wx.CENTER, 1)
            sizer = sizer_center
            self.SetSizer(sizer)
            self.wizard = wizard
            self.radio = radio

        def GetNext(self):
            next_classes = [
                RegularFile,
                ExistingDevice,
            ]
            return next_classes[self.radio.GetSelection()](self, self.wizard)

    wizard = wxWizard.wxWizard(mainframe, title="Add a new virtual disk")
    page = FirstPage(wizard)
    wizard.FitToPage(page)
    wizard.SetSize(wx.Size(200,100))
    result = wizard.RunWizard(page)
    if not result:
        data = None
    if data:
        return XMLWrapper(data)

if __name__ == '__main__':
    run_wizard(wx.Frame(wx.NULL, -1, "test"))
