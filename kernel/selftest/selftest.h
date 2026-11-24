/*
 * Fobos OS — каркас самопроверок.
 * Запускается: (а) в ядре при каждой загрузке FND-1 (kselftest),
 * (б) на host-инструментарии (tests/host) для быстрой итерации.
 * Предусловие: phys_init() уже выполнен (для phys-тестов).
 */
#ifndef FOBOS_SELFTEST_H
#define FOBOS_SELFTEST_H

/* Возвращает число упавших тестов (0 = успех). Итоги логируются. */
int kselftest_run(void);

/* Тесты, требующие хранилища (FAT32 e2e на реальном диске QEMU).
 * Вызывается из kmain ПОСЛЕ успешного storage_init(). На host не
 * выполняется (нет диска). */
int kselftest_storage_run(void);

#endif /* FOBOS_SELFTEST_H */
