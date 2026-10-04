idf_component_register(SRCS "ferry.c"
                            "ble_hid.c"
                            "control.c"
                            "cursor.c"
                            "diag.c"
                            "kvm.c"
                            "layout.c"
                            "our_descriptor.c"
                            "usb_input.c"
                       INCLUDE_DIRS ".")

# Stops NimBLE's automatic data length request on connect; see ble_hid.c.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_hs_hci_util_set_data_len")
