use ha_model::canonical::{
    AbiError, AutomationKey, GroupAutomationKey, GroupItemKey, GroupItemRecord, GroupKey,
    GroupRecord, RuleAction, RuleCommandValue, RuleRecord, RuleTrigger, GROUP_ITEM_RESERVED_OFFSET,
    RULE_ACTIONS_OFFSET, RULE_ACTION_COUNT_OFFSET, RULE_ACTION_RESERVED_OFFSET,
    RULE_CONDITIONS_COUNT_OFFSET, RULE_CONDITIONS_OFFSET, RULE_CONDITION_COMPARE_OFFSET,
    RULE_CONDITION_RESERVED_OFFSET, RULE_REF_RESERVED_OFFSET, RULE_RESERVED_OFFSET,
    RULE_TRIGGERS_OFFSET, RULE_TRIGGER_COUNT_OFFSET, RULE_TRIGGER_RESERVED_OFFSET,
};
use ha_model::Value;

const RULE: &[u8; 456] = include_bytes!("fixtures/rule.bin");
const GROUP_KEY: &[u8; 8] = include_bytes!("fixtures/group_key.bin");
const GROUP_RECORD: &[u8; 32] = include_bytes!("fixtures/group_record.bin");
const GROUP_ITEM_KEY: &[u8; 16] = include_bytes!("fixtures/group_item_key.bin");
const GROUP_ITEM_RECORD: &[u8; 36] = include_bytes!("fixtures/group_item_record.bin");
const GROUP_AUTOMATION_KEY: &[u8; 16] = include_bytes!("fixtures/group_automation_key.bin");
const AUTOMATION_KEY: &[u8; 8] = include_bytes!("fixtures/automation_key.bin");

#[test]
fn v2_rule_fixture_decodes_and_encodes_exactly() {
    let rule = RuleRecord::decode(RULE).unwrap();

    assert_eq!(rule.enabled, 1);
    assert_eq!(rule.trigger_count, 4);
    assert_eq!(rule.action_count, 4);
    assert_eq!(rule.conditions_count, 4);
    assert_eq!(&rule.name[..14], b"Canonical rule");
    assert!(rule.name[14..].iter().all(|byte| *byte == 0));

    assert_eq!(rule.triggers[0].entity, 0x1122_3344_5566_7788);
    assert_eq!(rule.triggers[0].event_id, 1);
    assert_eq!(rule.triggers[0].kind, 0);
    assert_eq!(rule.triggers[1].entity, 0x8877_6655_4433_2211);
    assert_eq!(rule.triggers[1].property, 8);
    assert_eq!(rule.triggers[1].value, 21.5);
    assert_eq!(rule.triggers[1].value2, 25.0);
    assert_eq!(rule.triggers[1].kind, 2);
    assert_eq!(rule.triggers[1].op, 8);
    assert_eq!(rule.triggers[1].edge, 1);
    assert_eq!(rule.triggers[2].minutes_of_day, 450);
    assert_eq!(rule.triggers[2].kind, 1);
    assert_eq!(rule.triggers[2].weekday_mask, 0x1F);
    assert_eq!(rule.triggers[3].kind, 2);
    assert_eq!(rule.triggers[3].op, 1);
    assert_eq!(rule.triggers[3].edge, 2);

    assert_eq!(rule.actions[0].reference.entity, 0x1122_3344_5566_7788);
    assert_eq!(rule.actions[0].reference.property, 2);
    assert_eq!(rule.actions[0].value, RuleCommandValue::None);
    assert_eq!(rule.actions[0].action, 4);
    assert_eq!(
        rule.actions[1].value,
        RuleCommandValue::Scalar(Value::u32(75))
    );
    assert_eq!(
        rule.actions[2].value,
        RuleCommandValue::Xy { x: 0.25, y: 0.5 }
    );
    assert_eq!(rule.actions[2].reference.entity, u64::MAX);
    assert_eq!(rule.actions[3].action, 3);

    assert_eq!(rule.conditions[0].reference.property, 8);
    assert_eq!(rule.conditions[0].compare.op, 8);
    assert_eq!(rule.conditions[0].compare.value, 18.0);
    assert_eq!(rule.conditions[0].compare.value2, 24.0);
    assert_eq!(rule.conditions[1].compare.op, 5);
    assert_eq!(rule.conditions[1].compare.value, 7.0);
    assert_eq!(rule.conditions[2].compare.op, 6);
    assert_eq!(rule.conditions[3].reference.entity, 0x1122_3344_5566_7788);

    assert_eq!(&rule.encode().unwrap()[..], &RULE[..]);
}

#[test]
fn full_rule_name_is_preserved() {
    let mut rule = RuleRecord::decode(RULE).unwrap();
    for (index, byte) in rule.name.iter_mut().enumerate() {
        *byte = index as u8 + 1;
    }
    let encoded = rule.encode().unwrap();
    let decoded = RuleRecord::decode(&encoded).unwrap();
    assert_eq!(decoded.name, rule.name);
}

#[test]
fn v2_group_fixtures_decode_and_encode_exactly() {
    let key = GroupKey::from_bytes(GROUP_KEY).unwrap();
    assert_eq!(key.id, 0x0102_0304_0506_0708);
    assert_eq!(&key.to_bytes()[..], &GROUP_KEY[..]);

    let record = GroupRecord::decode(GROUP_RECORD).unwrap();
    assert_eq!(&record.title[..15], b"Canonical group");
    assert_eq!(&record.encode()[..], &GROUP_RECORD[..]);

    let item_key = GroupItemKey::from_bytes(GROUP_ITEM_KEY).unwrap();
    assert_eq!(item_key.group_id, 0x0102_0304_0506_0708);
    assert_eq!(item_key.entity, 0x1122_3344_5566_7788);
    assert_eq!(&item_key.to_bytes()[..], &GROUP_ITEM_KEY[..]);

    let item = GroupItemRecord::decode(GROUP_ITEM_RECORD).unwrap();
    assert_eq!(item.order, 7);
    assert_eq!(&item.title[..14], b"Canonical item");
    assert_eq!(&item.encode()[..], &GROUP_ITEM_RECORD[..]);

    let group_automation = GroupAutomationKey::from_bytes(GROUP_AUTOMATION_KEY).unwrap();
    assert_eq!(group_automation.group_id, 0x0102_0304_0506_0708);
    assert_eq!(group_automation.automation_id, 0x1112_1314_1516_1718);
    assert_eq!(&group_automation.to_bytes()[..], &GROUP_AUTOMATION_KEY[..]);

    let automation = AutomationKey::from_bytes(AUTOMATION_KEY).unwrap();
    assert_eq!(automation.id, 0x1112_1314_1516_1718);
    assert_eq!(&automation.to_bytes()[..], &AUTOMATION_KEY[..]);
}

#[test]
fn nonzero_reserved_is_rejected() {
    let offsets = [
        RULE_RESERVED_OFFSET,
        RULE_TRIGGERS_OFFSET + RULE_TRIGGER_RESERVED_OFFSET,
        RULE_ACTIONS_OFFSET + RULE_REF_RESERVED_OFFSET,
        RULE_ACTIONS_OFFSET + RULE_ACTION_RESERVED_OFFSET,
        RULE_CONDITIONS_OFFSET + RULE_REF_RESERVED_OFFSET,
        RULE_CONDITIONS_OFFSET + RULE_CONDITION_COMPARE_OFFSET + 1,
        RULE_CONDITIONS_OFFSET + RULE_CONDITION_RESERVED_OFFSET,
    ];
    for offset in offsets {
        let mut bytes = *RULE;
        bytes[offset] = 1;
        assert_eq!(RuleRecord::decode(&bytes), Err(AbiError::NonZeroReserved));
    }

    let mut bytes = *GROUP_ITEM_RECORD;
    bytes[GROUP_ITEM_RESERVED_OFFSET] = 1;
    assert_eq!(
        GroupItemRecord::decode(&bytes),
        Err(AbiError::NonZeroReserved)
    );
}

#[test]
fn counts_above_four_are_rejected() {
    for offset in [
        RULE_TRIGGER_COUNT_OFFSET,
        RULE_ACTION_COUNT_OFFSET,
        RULE_CONDITIONS_COUNT_OFFSET,
    ] {
        let mut bytes = *RULE;
        bytes[offset] = 5;
        assert_eq!(RuleRecord::decode(&bytes), Err(AbiError::InvalidCount));
    }

    let mut rule = RuleRecord::decode(RULE).unwrap();
    rule.trigger_count = 5;
    assert_eq!(rule.encode(), Err(AbiError::InvalidCount));

    let mut rule = RuleRecord::decode(RULE).unwrap();
    rule.action_count = 5;
    assert_eq!(rule.encode(), Err(AbiError::InvalidCount));

    let mut rule = RuleRecord::decode(RULE).unwrap();
    rule.conditions_count = 5;
    assert_eq!(rule.encode(), Err(AbiError::InvalidCount));
}

#[test]
fn scalar_none_is_rejected() {
    let mut bytes = *RULE;
    let scalar_kind = RULE_ACTIONS_OFFSET + 32 + 16 + 4;
    bytes[scalar_kind..scalar_kind + 4].copy_from_slice(&0u32.to_le_bytes());
    assert_eq!(RuleRecord::decode(&bytes), Err(AbiError::InvalidKind));

    let mut rule = RuleRecord::decode(RULE).unwrap();
    rule.actions[1].value = RuleCommandValue::Scalar(Value::NONE);
    assert_eq!(rule.encode(), Err(AbiError::InvalidKind));
}

#[test]
fn truncated_and_unknown_values_are_rejected() {
    assert_eq!(RuleRecord::decode(&RULE[..455]), Err(AbiError::InvalidSize));
    assert_eq!(RuleTrigger::decode(&RULE[..31]), Err(AbiError::InvalidSize));
    assert_eq!(
        RuleAction::decode(&RULE[RULE_ACTIONS_OFFSET..RULE_ACTIONS_OFFSET + 31]),
        Err(AbiError::InvalidSize)
    );
    assert_eq!(
        GroupRecord::decode(&GROUP_RECORD[..31]),
        Err(AbiError::InvalidSize)
    );
    assert_eq!(
        GroupItemRecord::decode(&GROUP_ITEM_RECORD[..35]),
        Err(AbiError::InvalidSize)
    );

    let mut bytes = *RULE;
    bytes[RULE_ACTIONS_OFFSET + 16..RULE_ACTIONS_OFFSET + 20].copy_from_slice(&9u32.to_le_bytes());
    assert_eq!(RuleRecord::decode(&bytes), Err(AbiError::InvalidKind));

    let mut bytes = *RULE;
    let scalar_kind = RULE_ACTIONS_OFFSET + 32 + 16 + 4;
    bytes[scalar_kind..scalar_kind + 4].copy_from_slice(&9u32.to_le_bytes());
    assert_eq!(RuleRecord::decode(&bytes), Err(AbiError::InvalidKind));

    let mut bytes = *RULE;
    bytes[RULE_ACTIONS_OFFSET + 20] = 1;
    assert_eq!(RuleRecord::decode(&bytes), Err(AbiError::NonZeroUnused));
}
