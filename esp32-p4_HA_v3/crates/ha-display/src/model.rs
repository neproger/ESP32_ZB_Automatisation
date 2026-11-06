//! Canonical/domain reads and formatting helpers for the Display UI.

use super::*;

#[derive(Clone)]
pub(super) struct GroupRef {
    pub(super) id: u64,
    pub(super) title: String,
}

pub(super) fn c_string(bytes: &[u8]) -> String {
    let end = bytes
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

pub(super) fn read_state(
    domain: &DomainHandle,
    entity: EntityId,
    property: PropertyId,
) -> Option<Value> {
    let key = StateKey { entity, property }.to_bytes();
    let mut bytes = [0u8; STATE_RECORD_LEN];
    domain
        .entity_get(ENTITY_PROPERTY_STATE, &key, &mut bytes)
        .ok()?;
    Value::from_bytes(&bytes)
}

pub(super) fn collect_groups(domain: &DomainHandle) -> Vec<GroupRef> {
    let Ok(snapshot) = domain.entity_snapshot(ENTITY_GROUP) else {
        return Vec::new();
    };
    let mut groups = Vec::new();
    for index in 0..snapshot.len() {
        let (Some(key), Some(record)) = (snapshot.key(index), snapshot.record(index)) else {
            continue;
        };
        let (Ok(key), Ok(record)) = (GroupKey::from_bytes(key), GroupRecord::decode(record)) else {
            continue;
        };
        groups.push(GroupRef {
            id: key.id,
            title: c_string(&record.title),
        });
    }
    groups.sort_by(|a, b| a.title.cmp(&b.title));
    groups
}

pub(super) struct Item {
    pub(super) order: u16,
    pub(super) entity: EntityId,
    pub(super) title: String,
}

pub(super) fn collect_items(domain: &DomainHandle, group: u64) -> Vec<Item> {
    let Ok(snapshot) = domain.entity_snapshot(ENTITY_GROUP_ITEM) else {
        return Vec::new();
    };
    let mut items = Vec::new();
    for index in 0..snapshot.len() {
        let (Some(key), Some(record)) = (snapshot.key(index), snapshot.record(index)) else {
            continue;
        };
        let (Ok(key), Ok(record)) = (
            GroupItemKey::from_bytes(key),
            GroupItemRecord::decode(record),
        ) else {
            continue;
        };
        if key.group_id != group {
            continue;
        }
        items.push(Item {
            entity: key.entity,
            order: record.order,
            title: c_string(&record.title),
        });
    }
    items.sort_by_key(|item| item.order);
    items
}

pub(super) struct AutomationItem {
    pub(super) order: u16,
    pub(super) automation: u64,
    pub(super) title: String,
}

pub(super) fn collect_group_automations(domain: &DomainHandle, group: u64) -> Vec<AutomationItem> {
    let Ok(snapshot) = domain.entity_snapshot(ENTITY_GROUP_AUTOMATION) else {
        return Vec::new();
    };
    let mut items = Vec::new();
    for index in 0..snapshot.len() {
        let (Some(key), Some(record)) = (snapshot.key(index), snapshot.record(index)) else {
            continue;
        };
        let (Ok(key), Ok(record)) = (
            GroupAutomationKey::from_bytes(key),
            GroupItemRecord::decode(record),
        ) else {
            continue;
        };
        if key.group_id != group {
            continue;
        }
        items.push(AutomationItem {
            automation: key.automation_id,
            order: record.order,
            title: c_string(&record.title),
        });
    }
    items.sort_by_key(|item| item.order);
    items
}

pub(super) enum PageItem {
    Entity(Item),
    Automation(AutomationItem),
}

pub(super) fn collect_page_items(domain: &DomainHandle, group: u64) -> Vec<PageItem> {
    let mut entities = collect_items(domain, group);
    entities.reverse();
    let mut automations = collect_group_automations(domain, group);
    automations.reverse();
    let mut out = Vec::new();
    while !entities.is_empty() || !automations.is_empty() {
        let take_entity = match (entities.last(), automations.last()) {
            (Some(entity), Some(automation)) => entity.order <= automation.order,
            (Some(_), None) => true,
            _ => false,
        };
        if take_entity {
            out.push(PageItem::Entity(entities.pop().unwrap()));
        } else {
            out.push(PageItem::Automation(automations.pop().unwrap()));
        }
    }
    out
}

pub(super) fn automation_enabled(domain: &DomainHandle, automation: u64) -> bool {
    let mut bytes = [0u8; AUTOMATION_RECORD_LEN];
    if domain
        .entity_get(ENTITY_AUTOMATION, &automation.to_le_bytes(), &mut bytes)
        .is_err()
    {
        return false;
    }
    RuleRecord::decode(&bytes).is_ok_and(|rule| rule.enabled != 0)
}

pub(super) fn entity_name(domain: &DomainHandle, entity: EntityId) -> String {
    let mut bytes = [0u8; ha_domain::canonical::ENTITY_RECORD_LEN];
    if domain
        .entity_get(ENTITY_ENTITY, &entity.to_le_bytes(), &mut bytes)
        .is_err()
    {
        return String::new();
    }
    EntityRecord::decode(&bytes)
        .map(|record| c_string(&record.name))
        .unwrap_or_default()
}

pub(super) fn capabilities(domain: &DomainHandle, entity: EntityId) -> Vec<PropertyId> {
    let mut bytes = [0u8; CAPABILITIES_LEN];
    if domain
        .entity_get(ENTITY_CAPABILITIES, &entity.to_le_bytes(), &mut bytes)
        .is_err()
    {
        return Vec::new();
    }
    let Some(record) = CapabilitiesRecord::decode(&bytes) else {
        return Vec::new();
    };
    record.properties[..usize::from(record.property_count)]
        .iter()
        .map(|capability| capability.property)
        .filter(|property| *property != 0)
        .collect()
}

pub(super) fn numeric(value: &Value) -> Option<f32> {
    value
        .as_f32()
        .or_else(|| value.as_i32().map(|v| v as f32))
        .or_else(|| value.as_u32().map(|v| v as f32))
}

pub(super) fn property_name(property: PropertyId) -> &'static str {
    match property {
        PROPERTY_POWER => "Питание",
        PROPERTY_BRIGHTNESS => "Яркость",
        PROPERTY_COLOR => "Цвет",
        PROPERTY_COLOR_TEMPERATURE => "Температура цвета",
        PROPERTY_TEMPERATURE => "Температура",
        PROPERTY_HUMIDITY => "Влажность",
        PROPERTY_ILLUMINANCE => "Освещённость",
        PROPERTY_OCCUPANCY => "Занятость",
        PROPERTY_BATTERY_PERCENT => "Батарея",
        PROPERTY_BATTERY_VOLTAGE => "Напряжение",
        _ => "Свойство",
    }
}

pub(super) fn format_value(property: PropertyId, value: &Value) -> String {
    if property == PROPERTY_OCCUPANCY {
        return if value.as_bool().unwrap_or(false) {
            "Занято".into()
        } else {
            "Свободно".into()
        };
    }
    if let Some(on) = value.as_bool() {
        return if on {
            "Вкл".into()
        } else {
            "Выкл".into()
        };
    }
    let Some(number) = numeric(value) else {
        return "—".into();
    };
    match property {
        PROPERTY_TEMPERATURE => format!("{number:.1} °C"),
        PROPERTY_HUMIDITY => format!("{number:.1} %"),
        PROPERTY_ILLUMINANCE => format!("{number:.0} lx"),
        PROPERTY_BATTERY_PERCENT => format!("{number:.0} %"),
        PROPERTY_BATTERY_VOLTAGE => format!("{number:.2} V"),
        PROPERTY_BRIGHTNESS => format!("{number:.0} %"),
        PROPERTY_COLOR_TEMPERATURE => format!("{number:.0} K"),
        _ => format!("{number:.1}"),
    }
}

pub(super) fn kind_for(property: PropertyId) -> WidgetKind {
    match property {
        PROPERTY_POWER => WidgetKind::Switch,
        PROPERTY_BRIGHTNESS | PROPERTY_COLOR_TEMPERATURE => WidgetKind::Slider,
        _ => WidgetKind::Value,
    }
}

/* --- sRGB <-> linear <-> HSV <-> CIE xy (порт ui_color.c) --------------- */
