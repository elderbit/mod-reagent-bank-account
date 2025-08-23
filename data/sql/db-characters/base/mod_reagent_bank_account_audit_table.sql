CREATE TABLE
    IF NOT EXISTS `mod_reagent_bank_audit` (
        `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
        `ts` INT NOT NULL,
        `account_id` INT NOT NULL,
        `guid` INT NOT NULL,
        `action` ENUM ('DEPOSIT', 'WITHDRAW') NOT NULL,
        `item_entry` INT NOT NULL,
        `item_subclass` INT NOT NULL,
        `delta` INT NOT NULL,
        PRIMARY KEY (`id`),
        KEY `idx_ts` (`ts`),
        KEY `idx_account_guid` (`account_id`, `guid`),
        KEY `idx_item` (`item_entry`)
    ) ENGINE = InnoDB DEFAULT CHARSET = UTF8MB4;
