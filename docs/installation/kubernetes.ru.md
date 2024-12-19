[Документация](../../README-ru.md#документация) → Установка → Kubernetes CSI

-----

[Read in English](kubernetes.en.md)

# Kubernetes CSI

У Vitastor есть CSI-плагин для Kubernetes, поддерживающий блочные тома и тома на основе
кластерной ФС VitastorFS.

Блочные тома могут быть отформатированы и примонтированы со стандартной ФС (ext4 или xfs).
Такие тома поддерживают только режим RWO (ReadWriteOnce, одновременный доступ с одного узла).

Блочные тома также могут не форматироваться и подключаться в контейнер в виде блочного устройства.
В таком случае их можно подключать в режиме RWX (ReadWriteMany, одновременный доступ с многих узлов).

Тома на основе VitastorFS используют кластерную ФС и поэтому также поддерживают режим RWX
(ReadWriteMany). Однако, такие тома не поддерживают ограничение размера и снимки.

Для установки возьмите манифесты из директории [csi/deploy/](../../csi/deploy/), поместите
вашу конфигурацию подключения к Vitastor в [csi/deploy/001-csi-config-map.yaml](../../csi/deploy/001-csi-config-map.yaml),
настройте StorageClass в [csi/deploy/009-storage-class.yaml](../../csi/deploy/009-storage-class.yaml)
и примените все `NNN-*.yaml` к вашей инсталляции Kubernetes.

```
for i in ./???-*.yaml; do kubectl apply -f $i; done
```

После этого вы сможете создавать PersistentVolume.

**Важно:** Лучше всего использовать ядро Linux версии не менее 5.15 с включёнными модулями
[VDUSE](../usage/qemu.ru.md#vduse) (vdpa, vduse, virtio-vdpa). Если в вашем дистрибутиве
они не собраны из коробки - соберите их сами, обещаю, что это стоит того ([инструкция](../usage/qemu.ru.md#vduse)) :-).
Когда VDUSE недоступно, CSI-плагин использует [NBD](../usage/nbd.ru.md) для подключения
дисков, а NBD медленнее и имеет проблему таймаута - если кластер остаётся недоступным
дольше, чем [nbd_timeout](../config/client.ru.md#nbd_timeout), NBD-устройство отключается
и ломает поды, использующие его.

## Возможности

CSI-плагин Vitastor поддерживает:
- Версии Kubernetes, начиная с 1.20 (или с 1.17 для более старых vitastor-csi <= 1.1.0)
- Файловые RWO (ReadWriteOnce) тома. Пример: [PVC](../../csi/deploy/example-pvc.yaml), [под](../../csi/deploy/example-test-pod.yaml)
- Сырые блочные RWX (ReadWriteMany) тома. Пример: [PVC](../../csi/deploy/example-pvc-block.yaml), [под](../../csi/deploy/example-test-pod-block.yaml)
- Основанные на VitastorFS RWX (ReadWriteMany) тома. Пример: [класс хранения](../../csi/deploy/example-storage-class-fs.yaml)
- Расширение размера томов
- Снимки томов. Пример: [класс снимков](../../csi/deploy/example-snapshot-class.yaml), [снимок](../../csi/deploy/example-snapshot.yaml), [клон снимка](../../csi/deploy/example-snapshot-clone.yaml)
- Способы подключения устройств [VDUSE](../usage/qemu.ru.md#vduse) (предпочитаемый) и [NBD](../usage/nbd.ru.md)
- Обновление при использовании VDUSE - новые процессы-обработчики устройств успешно перезапускаются вместе с самими подами CSI
- Автоперезауск демонов VDUSE - процесс-обработчик автоматически перезапустится, если он внезапно упадёт из-за бага в коде клиента Vitastor
- Несколько кластеров через задание нескольких файлов конфигурации в ConfigMap.

Не забывайте, что для использования снимков нужно сначала установить [контроллер снимков и CRD](https://kubernetes-csi.github.io/docs/snapshot-controller.html#deployment).
