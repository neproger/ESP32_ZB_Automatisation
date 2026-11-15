    use super::*;

    #[test]
    fn siphash_matches_official_vector() {
        let mut key = [0u8; 16];
        let mut msg = [0u8; 15];
        for (i, byte) in key.iter_mut().enumerate() {
            *byte = i as u8;
        }
        for (i, byte) in msg.iter_mut().enumerate() {
            *byte = i as u8;
        }
        assert_eq!(siphash24(&key, &msg), 0xa129_ca61_49be_45e5);
    }

    #[test]
    fn zigbee_entity_id_matches_v2_golden() {
        let id = zigbee_entity_id(0x0012_4B00_0A1B_2C3D, 1);
        assert_eq!(id, 0xdf80_3cba_f588_876d);
        assert_ne!(id, zigbee_entity_id(0x0012_4B00_0A1B_2C3D, 2));
        assert_ne!(entity_id_derive(ISSUER_SYSTEM, &zigbee_seed(1, 1)), id);
    }

    #[test]
    fn wifi_records_match_v2_layouts() {
        let mut scan = WifiScanRecord {
            count: 1,
            ..WifiScanRecord::default()
        };
        scan.aps[0].ssid[..4].copy_from_slice(b"wifi");
        scan.aps[0].rssi = -54;
        scan.aps[0].auth = 2;
        let bytes = scan.encode();
        assert_eq!(bytes.len(), WIFI_SCAN_RECORD_LEN);
        assert_eq!(WifiScanRecord::decode(&bytes), Some(scan));

        let status = WifiStatusRecord {
            state: 3,
            connected: 1,
            rssi: -60,
            ssid: scan.aps[0].ssid,
        };
        let bytes = status.encode();
        assert_eq!(bytes.len(), WIFI_STATUS_RECORD_LEN);
        assert_eq!(WifiStatusRecord::decode(&bytes), Some(status));

        let known = WifiKnownRecord {
            ssid: scan.aps[0].ssid,
            password: [b'x'; WIFI_PASSWORD_MAX],
        };
        let bytes = known.encode();
        assert_eq!(bytes.len(), WIFI_KNOWN_RECORD_LEN);
        assert_eq!(WifiKnownRecord::decode(&bytes), Some(known));
    }

    #[test]
    fn wifi_connect_args_roundtrip_and_truncate() {
        let bytes = encode_wifi_connect("HomeNet", "secret");
        assert_eq!(bytes.len(), WIFI_CONNECT_ARGS_LEN);
        assert_eq!(
            decode_wifi_connect(&bytes),
            Some(("HomeNet".to_string(), "secret".to_string()))
        );

        let long = "x".repeat(100);
        let bytes = encode_wifi_connect(&long, &long);
        let (ssid, password) = decode_wifi_connect(&bytes).unwrap();
        assert_eq!(ssid.len(), WIFI_SSID_MAX - 1);
        assert_eq!(password.len(), WIFI_PASSWORD_MAX - 1);
    }

    #[test]
    fn source_and_command_ids_match_v2() {
        assert_eq!(SOURCE_UI, 2);
        assert_eq!(SOURCE_AUTOMATION, 3);
        assert_eq!(SOURCE_SYSTEM, 4);
        assert_eq!(SOURCE_WIFI, 5);
        assert_eq!(COMMAND_PROPERTY, 6);
    }

    #[test]
    fn property_command_roundtrips_in_v2_layout() {
        for value in [
            CommandValue::None,
            CommandValue::Scalar(Value::bool(true)),
            CommandValue::Scalar(Value::i32(-7)),
            CommandValue::Scalar(Value::u32(42)),
            CommandValue::Scalar(Value::f32(50.0)),
            CommandValue::Scalar(Value::enum_(3)),
            CommandValue::Xy { x: 0.25, y: 0.5 },
        ] {
            let command = CanonicalCommand {
                entity: 0x1122_3344_5566_7788,
                property: PROPERTY_BRIGHTNESS,
                action: ACTION_SET,
                value,
            };
            let bytes = command.encode();
            assert_eq!(bytes.len(), COMMAND_RECORD_LEN);
            assert_eq!(CanonicalCommand::decode(&bytes), Some(command));
        }
        assert!(CanonicalCommand::decode(&[0u8; COMMAND_RECORD_LEN - 1]).is_none());
    }

    #[test]
    fn records_have_v2_sizes_and_layouts() {
        let entity = EntityRecord::new(7, "Kitchen");
        assert_eq!(entity.encode().len(), ENTITY_RECORD_LEN);
        let device = LogicalDeviceRecord::new(SOURCE_ZIGBEE, 0, "00124b000a1b2c3d", "Plug");
        assert_eq!(device.encode().len(), DEVICE_RECORD_LEN);
        let key = StateKey {
            entity: 1,
            property: PROPERTY_POWER,
        };
        assert_eq!(key.to_bytes().len(), STATE_KEY_LEN);
        let record = StateRecord {
            value: Value::bool(true),
        };
        assert_eq!(record.encode().len(), STATE_RECORD_LEN);
        assert_eq!(record.encode()[0], 1);
        let mut caps = CapabilitiesRecord::default();
        assert!(caps.push(Capability::with_actions(
            PROPERTY_POWER,
            &[ACTION_ON, ACTION_OFF, ACTION_TOGGLE]
        )));
        assert!(caps.push_event(EVENT_SINGLE_PRESS));
        assert_eq!(caps.encode().len(), CAPABILITIES_LEN);
    }

    #[test]
    fn display_decoders_roundtrip() {
        let entity = EntityRecord::new(7, "lamp");
        assert_eq!(EntityRecord::decode(&entity.encode()).unwrap(), entity);
        let key = StateKey {
            entity: 7,
            property: 1,
        };
        assert_eq!(StateKey::from_bytes(&key.to_bytes()).unwrap(), key);
        let group = GroupRecord {
            title: [b'G'; GROUP_TITLE_MAX],
        };
        assert_eq!(GroupRecord::decode(&group.encode()).unwrap(), group);
        let mut caps = CapabilitiesRecord::default();
        caps.push(Capability::with_actions(1, &[1, 2]));
        assert_eq!(CapabilitiesRecord::decode(&caps.encode()).unwrap(), caps);
    }
    #[test]
    fn location_layout_and_flags_match_v2() {
        let mut name = [0u8; LOCATION_NAME_MAX];
        name[..6].copy_from_slice(b"Moscow");
        let location = LocationRecord {
            latitude: 1.5,
            longitude: -2.25,
            tz_offset_min: 180,
            tz_auto: false,
            pos_auto: false,
            name,
        };
        let bytes = location.encode();
        assert_eq!(bytes.len(), LOCATION_RECORD_LEN);
        assert_eq!(bytes[10], 0);
        assert_eq!(LocationRecord::decode(&bytes), Some(location));
        assert_eq!(location.effective_tz_offset_min(), 180);

        let auto = LocationRecord::default();
        assert_eq!(auto.encode()[10], 3);
        assert_eq!(auto.effective_tz_offset_min(), 0);
        assert!(LocationRecord::decode(&bytes[..59]).is_none());
    }

    #[test]
    fn weather_layout_matches_v2() {
        let weather = WeatherRecord {
            condition: WEATHER_PARTLYCLOUDY,
            cloud_pct: 40,
            temperature_c100: 2_150,
            humidity_p100: 6_100,
            pressure_hpa: 1_013,
            wind_kmh10: 125,
            wind_dir_deg: 270,
        };
        let bytes = weather.encode();
        assert_eq!(bytes.len(), WEATHER_RECORD_LEN);
        assert_eq!(&bytes[2..4], &2_150i16.to_le_bytes());
        assert_eq!(WeatherRecord::decode(&bytes), Some(weather));
        assert!(WeatherRecord::decode(&bytes[..15]).is_none());
    }
