import wx, sys
from common import *

class NetworkDevicesOptionArray(OptionArrayPanel):
    CLASS_DESC = 'Virtual Network Adapters'
    SHORT_TREE_DESC = 'Networking'

    def get_item_list(self):
        return self.mainframe.network_devices

    class Item(ConfigurationItem):
        def title(self, long=False):
            device = self._xml_item
            index = int(device.attr.index)
            if long:
                name = 'conet%d' % (index, )
            else:
                name = '[%d]' % (index, )
            return '%s: %s' % (name, device.attr.type)

    class ItemPanel(OptionArrayPanel.ItemPanel):
        types_dict = {
            'TAP Win32' : 'tap',
            'Bridged Ethernet' : 'bridged',
        }
        types_dict_rev = dict([(a, b) for b, a in types_dict.items()])

        def populate(self, panel_sizer):
            sizer = wx.BoxSizer(wx.VERTICAL)

            sizer2 = wx.BoxSizer(wx.HORIZONTAL)
            text = wx.StaticText(self.panel, -1, 'Adapter Type: ')
            sizer2.Add(text, 0, 0, 3)
            current_type = self.types_dict_rev[str(self.item._xml_item.attr.type)]
            if sys.platform == 'win32':
                style = 0
            else:
                style = wx.CB_READONLY
            comboctrl = wx.ComboBox(self.panel, -1, current_type, style=style)
            def type_combo_changed(event):
                self.load_adapter_list()
                self.changed(event)
            wx.EVT_COMBOBOX(comboctrl, -1, type_combo_changed)
            wx.EVT_TEXT(comboctrl, -1, type_combo_changed)
            [comboctrl.Append(typename) for typename in self.types_dict.keys()]
            sizer2.Add(comboctrl, 1, wx.EXPAND, 3)
            sizer.Add(sizer2, 0, wx.EXPAND | wx.ALL, 3)
            self.type_combo = comboctrl

            sizer2 = wx.BoxSizer(wx.HORIZONTAL)
            text = wx.StaticText(self.panel, -1, 'Adapter Name: ')
            sizer2.Add(text, 0, 0, 3)
            comboctrl = wx.ComboBox(self.panel, -1)
            wx.EVT_COMBOBOX(comboctrl, -1, self.changed)
            wx.EVT_TEXT(comboctrl, -1, self.changed)
            sizer2.Add(comboctrl, 1, wx.EXPAND, 3)
            sizer.Add(sizer2, 0, wx.EXPAND | wx.ALL, 3)
            self.name_combo = comboctrl
            self.load_adapter_list(False)
            comboctrl.SetValue(str(self.item._xml_item.attr.name))

            sizer2 = wx.BoxSizer(wx.HORIZONTAL)
            text = wx.StaticText(self.panel, -1, 'Index: ')
            sizer2.Add(text, 0, 0, 3)
            self.index_text = textctrl = wx.TextCtrl(self.panel, -1, self.item._xml_item.attr.index)
            wx.EVT_TEXT(textctrl, -1, self.changed)
            sizer2.Add(textctrl, 0, 0, 3)
            sizer.Add(sizer2, 0, wx.EXPAND | wx.ALL, 3)

            panel_sizer.Add(sizer, 0, wx.EXPAND | wx.ALL)

        def load_adapter_list(self, set_name=True):
            comboctrl = self.name_combo
            comboctrl.Clear()
            type = self.types_dict[self.type_combo.GetValue()]
            adapter_list = get_adapter_list(type)
            [comboctrl.Append(typename) for typename in adapter_list]
            if adapter_list and set_name:
                comboctrl.SetValue(adapter_list[0])

        def apply(self, event=None):
            try:
                ret_index = self.check_index(self.index_text.GetValue(), self.item._xml_item.attr.index)
            except InvalidArrayIndexChosen, e:
                wx.MessageDialog(self.panel, e.args[0], style=wx.OK).ShowModal()
                return False
            self.item._xml_item.attr.index = str(int(self.index_text.GetValue()))
            self.item._xml_item.attr.name = str(self.name_combo.GetValue())
            self.item._xml_item.attr.type = str(self.types_dict[self.type_combo.GetValue()])
            super(NetworkDevicesOptionArray.ItemPanel, self).apply(event)

TAPNAME = 'tap0801co'

def get_adapter_list(listtype):
    if sys.platform != 'win32':
        return []
    if listtype == 'tap':
        return [item for item, itype in win32_get_adapter_list() if itype == TAPNAME]
    return [item for item, itype in win32_get_adapter_list() if itype != TAPNAME]

def win32_get_adapter_list():
    """Get the list of network adapters an their corresponding
    network driver identifier."""

    from win32api import RegOpenKeyEx, RegEnumKeyEx
    from win32api import RegQueryValueEx
    from win32con import HKEY_LOCAL_MACHINE

    TAPNAME = 'tap08010co'
    NET_GUID = "{4D36E972-E325-11CE-BFC1-08002BE10318}"
    CONTROL_PATH = "SYSTEM\\CurrentControlSet\\Control\\"
    ADAPTER_KEY = CONTROL_PATH + "Class\\" + NET_GUID
    NETWORK_CONNECTIONS_KEY = CONTROL_PATH + "Network\\" + NET_GUID
    adapters = []
    adapter_types = {}

    key = RegOpenKeyEx(HKEY_LOCAL_MACHINE, ADAPTER_KEY)
    for value in RegEnumKeyEx(key):
        subpath = "\\".join([ADAPTER_KEY, value[0]])
        subkey = RegOpenKeyEx(HKEY_LOCAL_MACHINE, subpath)
        drivername = RegQueryValueEx(subkey, "ComponentId")
        instanceid = RegQueryValueEx(subkey, "NetCfgInstanceId")
        adapter_types[instanceid[0]] = drivername[0]

    key = RegOpenKeyEx(HKEY_LOCAL_MACHINE, NETWORK_CONNECTIONS_KEY)
    for value in RegEnumKeyEx(key):
        if value[0].startswith('{'):
            subpath = "\\".join([NETWORK_CONNECTIONS_KEY, value[0], "Connection"])
            subkey = RegOpenKeyEx(HKEY_LOCAL_MACHINE, subpath)
            name = RegQueryValueEx(subkey, "Name")
            if value[0] in adapter_types:
                adapters.append((name[0], adapter_types[value[0]]))

    return adapters

if __name__ == "__main__":
    print(get_adapter_list())
    pass
